#!/usr/bin/env python3
"""Verify SYNTH protocol schemas, fixtures, manifest, and a mirrored peer tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / "protocol"
V1 = PROTOCOL / "v1"
MANIFEST = V1 / "manifest.sha256"
VERSIONS = (V1, PROTOCOL / "v2")


class ValidationError(ValueError):
    pass


def unique_pairs(items: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in items:
        if key in result:
            raise ValidationError(f"duplicate key {key!r}")
        result[key] = value
    return result


def parse_json(text: str) -> Any:
    return json.loads(
        text,
        object_pairs_hook=unique_pairs,
        parse_constant=lambda value: (_ for _ in ()).throw(ValidationError(f"invalid number {value}")),
    )


def load_json(path: Path) -> Any:
    return parse_json(path.read_text(encoding="utf-8"))


def resolve_ref(ref: str, schema_path: Path) -> tuple[dict[str, Any], Path]:
    if ref.startswith("#"):
        document = load_json(schema_path)
        node: Any = document
        for part in ref.removeprefix("#/").split("/") if ref != "#" else []:
            node = node[part.replace("~1", "/").replace("~0", "~")]
        return node, schema_path
    path, separator, fragment = ref.partition("#")
    target = (schema_path.parent / path).resolve()
    if PROTOCOL.resolve() not in target.parents:
        raise ValidationError(f"reference escapes protocol tree: {ref}")
    if separator:
        return resolve_ref("#" + fragment, target)
    return load_json(target), target


def validate(value: Any, schema: dict[str, Any], schema_path: Path, at: str = "$") -> None:
    if "$ref" in schema:
        target, target_path = resolve_ref(schema["$ref"], schema_path)
        validate(value, target, target_path, at)
    for child in schema.get("allOf", []):
        validate(value, child, schema_path, at)
    if "anyOf" in schema:
        matches = 0
        for child in schema["anyOf"]:
            try:
                validate(value, child, schema_path, at)
                matches += 1
            except ValidationError:
                pass
        if matches == 0:
            raise ValidationError(f"{at}: value did not match any allowed schema")
    if "oneOf" in schema:
        matches = 0
        for child in schema["oneOf"]:
            try:
                validate(value, child, schema_path, at)
                matches += 1
            except ValidationError:
                pass
        if matches != 1:
            raise ValidationError(f"{at}: value matched {matches} oneOf schemas")
    if "not" in schema:
        try:
            validate(value, schema["not"], schema_path, at)
        except ValidationError:
            pass
        else:
            raise ValidationError(f"{at}: value matched a forbidden schema")
    if "if" in schema:
        try:
            validate(value, schema["if"], schema_path, at)
            condition = True
        except ValidationError:
            condition = False
        branch = schema.get("then" if condition else "else")
        if branch is not None:
            validate(value, branch, schema_path, at)
    if "const" in schema and value != schema["const"]:
        raise ValidationError(f"{at}: expected constant {schema['const']!r}")
    if "enum" in schema and value not in schema["enum"]:
        raise ValidationError(f"{at}: value is outside enum")
    kind = schema.get("type")
    type_ok = {
        "object": lambda: isinstance(value, dict),
        "array": lambda: isinstance(value, list),
        "string": lambda: isinstance(value, str),
        "integer": lambda: isinstance(value, int) and not isinstance(value, bool),
        "number": lambda: isinstance(value, (int, float)) and not isinstance(value, bool),
        "boolean": lambda: isinstance(value, bool),
        "null": lambda: value is None,
    }
    if kind and (kind not in type_ok or not type_ok[kind]()):
        raise ValidationError(f"{at}: expected {kind}")
    if isinstance(value, dict):
        required = schema.get("required", [])
        missing = [key for key in required if key not in value]
        if missing:
            raise ValidationError(f"{at}: missing {missing}")
        properties = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            unknown = sorted(set(value) - set(properties))
            if unknown:
                raise ValidationError(f"{at}: unknown properties {unknown}")
        if "maxProperties" in schema and len(value) > schema["maxProperties"]:
            raise ValidationError(f"{at}: too many properties")
        for key, child in value.items():
            if key in properties:
                validate(child, properties[key], schema_path, f"{at}.{key}")
    if isinstance(value, list):
        if "minItems" in schema and len(value) < schema["minItems"]:
            raise ValidationError(f"{at}: too few items")
        if "contains" in schema:
            matched = False
            for item in value:
                try:
                    validate(item, schema["contains"], schema_path, at)
                    matched = True
                    break
                except ValidationError:
                    pass
            if not matched:
                raise ValidationError(f"{at}: missing required array member")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            raise ValidationError(f"{at}: too many items")
        if schema.get("uniqueItems"):
            encoded = [json.dumps(item, sort_keys=True, separators=(",", ":")) for item in value]
            if len(encoded) != len(set(encoded)):
                raise ValidationError(f"{at}: duplicate items")
        if "items" in schema:
            for index, child in enumerate(value):
                validate(child, schema["items"], schema_path, f"{at}[{index}]")
    if isinstance(value, str):
        if "minLength" in schema and len(value) < schema["minLength"]:
            raise ValidationError(f"{at}: string too short")
        if "maxLength" in schema and len(value) > schema["maxLength"]:
            raise ValidationError(f"{at}: string too long")
        if "pattern" in schema and re.fullmatch(schema["pattern"], value) is None:
            raise ValidationError(f"{at}: pattern mismatch")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            raise ValidationError(f"{at}: below minimum")
        if "maximum" in schema and value > schema["maximum"]:
            raise ValidationError(f"{at}: above maximum")


def schema_for_fixture(path: Path, value: dict[str, Any]) -> Path:
    if value.get("schema") == "synth.visual_context.capture.v2":
        return PROTOCOL / "v2/schemas/visual-context-capture.schema.json"
    if value.get("schema") == "synth.event.v2":
        return PROTOCOL / "v2/schemas/event.schema.json"
    if value.get("schema") == "synth.response.line.v2":
        return PROTOCOL / "v2/schemas/response-line.schema.json"
    if value.get("schema") == "synth.event.v1":
        return V1 / "schemas/event.schema.json"
    if value.get("schema") == "synth.response.line.v1":
        return V1 / "schemas/response-line.schema.json"
    if value.get("schema") == "synth.media.v1":
        return V1 / "schemas/media.schema.json"
    if value.get("schema") == "synth.visual_context.capture.v1":
        return V1 / "schemas/visual-context-capture.schema.json"
    if value.get("schema") == "synth.visual_context.response.v1":
        return V1 / "schemas/visual-context-response.schema.json"
    raise ValidationError(f"{path}: unknown fixture schema")


def validate_payloads(value: dict[str, Any], path: Path) -> None:
    if value.get("schema") in {
        "synth.media.v1",
        "synth.visual_context.capture.v1",
        "synth.visual_context.capture.v2",
        "synth.visual_context.response.v1",
    }:
        return
    catalog_path = V1 / "schemas/payloads.schema.json"
    catalog = load_json(catalog_path)
    payload_type = value.get("type")
    payload_schema = catalog.get("$defs", {}).get(payload_type)
    if value.get("schema") == "synth.event.v2" and payload_type == "control":
        catalog_path = PROTOCOL / "v2/schemas/control.schema.json"
        payload_schema = load_json(catalog_path)
    if value.get("schema") == "synth.event.v2" and payload_type in {"input_text", "input_audio"}:
        payload_schema = dict(payload_schema)
        payload_schema["properties"] = dict(payload_schema["properties"])
        payload_schema["properties"]["control"] = {"$ref": "../../v2/schemas/control.schema.json#/$defs/selection"}
    if value.get("schema") == "synth.event.v2" and payload_type == "context":
        catalog_path = PROTOCOL / "v2/schemas/context.schema.json"
        payload_schema = load_json(catalog_path)
    if value.get("schema") == "synth.event.v2" and payload_type == "trigger":
        catalog_path = PROTOCOL / "v2/schemas/trigger.schema.json"
        payload_schema = load_json(catalog_path)
    if value.get("schema") == "synth.event.v2" and payload_type == "diary_request":
        catalog_path = PROTOCOL / "v2/schemas/diary-request.schema.json"
        payload_schema = load_json(catalog_path)
    if value.get("schema") == "synth.event.v2" and payload_type == "action_result":
        catalog_path = PROTOCOL / "v2/schemas/action-result-payload.schema.json"
        payload_schema = load_json(catalog_path)
    if payload_schema is None:
        raise ValidationError(f"{path}: no payload schema for {payload_type!r}")
    validate(value.get("payload"), payload_schema, catalog_path, f"{path}:payload")
    payload = value.get("payload", {})
    if payload_type == "action_result" and payload.get("result", {}).get("schema") == "synth.action.result.v2":
        if value.get("protocol_version") != 2 or not {"action.inventory_observation", "dialogue.turn_ownership"}.issubset(value.get("capabilities", [])):
            raise ValidationError(f"{path}: post-action inventory requires owned v2 capability")
        if len(payload["result"]["inventory"]["items"]) > 32 and (value.get("runtime_variant") != "flat" or "context.inventory_512" not in value.get("capabilities", [])):
            raise ValidationError(f"{path}: post-action extended inventory requires flat capability")
    if payload_type == "context" and any(len(state["inventory"]) > 32 for state in payload.get("actor_states", [])):
        if value.get("protocol_version") != 2 or value.get("runtime_variant") != "flat" or "context.inventory_512" not in value.get("capabilities", []):
            raise ValidationError(f"{path}: extended inventory requires v2 flat capability")
    if payload_type == "trigger" and payload.get("kind") == "player_reaction" and int(payload["event_id"][7:]) > 9007199254740991:
        raise ValidationError(f"{path}: player reaction ID exceeds wire-safe integer")
    if payload_type == "context" and "actor_events" in payload:
        batch = payload["actor_events"]
        if value.get("protocol_version") != 2 or value.get("runtime_variant") != "flat" or "context.actor_events" not in value.get("capabilities", []):
            raise ValidationError(f"{path}: native actor events require v2 flat capability")
        if int(batch["batch_id"][6:]) > 9007199254740991:
            raise ValidationError(f"{path}: actor batch ID exceeds wire-safe integer")
        for record in batch["events"]:
            other = record["other_actor"]
            if other and (other["form_id"].upper() != str(record["other_form_id"]).upper() or other["playthrough_id"] != record["actor"]["playthrough_id"]):
                raise ValidationError(f"{path}: secondary identity differs from original evidence")
    if payload_type == "context" and "player_event" in payload:
        event = payload["player_event"]
        before, after = event["before"], event["after"]
        if value.get("protocol_version") != 2 or value.get("runtime_variant") != "flat" or "context.player_events" not in value.get("capabilities", []):
            raise ValidationError(f"{path}: native player event requires v2 flat capability")
        if int(event["event_id"][7:]) > 9007199254740991 or after["game_time_ticks"] < before["game_time_ticks"] or after["level"] < before["level"]:
            raise ValidationError(f"{path}: invalid player transition ordering or ID")
        if event["kind"] == "level_up" and after["level"] <= before["level"]:
            raise ValidationError(f"{path}: level increase is unproven")
    if payload_type == "context" and "quest_events" in payload:
        if value.get("protocol_version") != 2 or value.get("runtime_variant") != "flat" or "context.quest_events" not in value.get("capabilities", []):
            raise ValidationError(f"{path}: native quest batch requires v2 flat capability")
        if any(event["visibility_sequence"] >= payload["snapshot_sequence"] for event in payload["quest_events"]["events"]):
            raise ValidationError(f"{path}: quest visibility witness must precede context or use zero")
    if payload_type == "context" and "loaded_context" in payload:
        if "context.saved_anchor" not in value.get("capabilities", []) or payload["loaded_context"]["playthrough_id"] != payload["playthrough_id"]:
            raise ValidationError(f"{path}: unnegotiated or cross-playthrough loaded context")


def verify_fixtures(version: Path = V1) -> int:
    checked = 0
    for path in sorted((version / "fixtures/valid").iterdir()):
        lines = path.read_text(encoding="utf-8").splitlines() if path.suffix == ".ndjson" else [path.read_text(encoding="utf-8")]
        values = []
        for line in lines:
            value = parse_json(line)
            schema_path = schema_for_fixture(path, value)
            validate(value, load_json(schema_path), schema_path, str(path))
            validate_payloads(value, path)
            values.append(value)
            checked += 1
        if path.suffix == ".ndjson":
            types = [value["type"] for value in values]
            if not types or types[0] != "response_start" or types[-1] != "response_end" or types.count("response_end") != 1:
                raise ValidationError(f"{path}: invalid stream lifecycle")
            correlation = {(v["request_id"], v["turn_id"], v["generation"], v["runtime_variant"], v["protocol_version"], v.get("context_sequence", 0)) for v in values}
            if len(correlation) != 1:
                raise ValidationError(f"{path}: inconsistent stream correlation")
            line_ids = [v["line_id"] for v in values]
            if len(line_ids) != len(set(line_ids)):
                raise ValidationError(f"{path}: duplicate line ids")
    for path in sorted((version / "fixtures/invalid").iterdir()):
        try:
            lines = path.read_text(encoding="utf-8").splitlines() if path.suffix == ".ndjson" else [path.read_text(encoding="utf-8")]
            for line in lines:
                value = parse_json(line)
                schema_path = schema_for_fixture(path, value)
                validate(value, load_json(schema_path), schema_path, str(path))
                validate_payloads(value, path)
        except (json.JSONDecodeError, ValidationError, TypeError):
            checked += 1
            continue
        raise ValidationError(f"{path}: invalid fixture unexpectedly passed")
    return checked


def verify_schema_closure(version: Path = V1) -> int:
    checked = 0
    for path in sorted((version / "schemas").glob("*.json")):
        value = load_json(path)
        if value.get("type") == "object" and value.get("additionalProperties") is not False:
            raise ValidationError(f"{path}: root object schema is not closed")
        checked += 1
    return checked


def protocol_files(root: Path) -> list[Path]:
    # pathlib's native ordering differs between Windows and Linux; the wire manifest must not.
    return sorted((path for path in root.rglob("*") if path.is_file() and path.name != "manifest.sha256"),
                  key=lambda path: path.relative_to(root).as_posix())


def generate_manifest(version: Path = V1) -> str:
    rows = []
    for path in protocol_files(version):
        rows.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(version).as_posix()}")
    return "\n".join(rows) + "\n"


def verify_canonical_bytes() -> int:
    checked = 0
    for path in protocol_files(PROTOCOL):
        data = path.read_bytes()
        if data.startswith(b"\xef\xbb\xbf") or b"\r" in data or not data.endswith(b"\n") or data.endswith(b"\n\n"):
            raise ValidationError(f"{path}: non-canonical text bytes")
        checked += 1
    for version in VERSIONS:
        expected = generate_manifest(version)
        manifest = version / "manifest.sha256"
        if not manifest.exists() or manifest.read_text(encoding="utf-8") != expected:
            raise ValidationError(f"{manifest} is missing or stale; run with --write-manifest")
    return checked


def compare_peer(peer: Path) -> int:
    peer = peer.resolve()
    left = {p.relative_to(PROTOCOL).as_posix(): p.read_bytes() for p in protocol_files(PROTOCOL)}
    right = {p.relative_to(peer).as_posix(): p.read_bytes() for p in protocol_files(peer)}
    if left.keys() != right.keys():
        raise ValidationError(f"peer path sets differ: only-local={sorted(left.keys()-right.keys())}, only-peer={sorted(right.keys()-left.keys())}")
    different = [path for path in left if left[path] != right[path]]
    if different:
        raise ValidationError(f"peer bytes differ: {different}")
    return len(left)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--peer", type=Path)
    parser.add_argument("--write-manifest", action="store_true")
    args = parser.parse_args()
    if args.write_manifest:
        for version in VERSIONS:
            (version / "manifest.sha256").write_text(generate_manifest(version), encoding="utf-8", newline="\n")
    schemas = sum(verify_schema_closure(version) for version in VERSIONS)
    fixtures = sum(verify_fixtures(version) for version in VERSIONS)
    files = verify_canonical_bytes()
    peers = compare_peer(args.peer) if args.peer else 0
    print(f"protocol verification passed: schemas={schemas} fixture-cases={fixtures} canonical-files={files} peer-files={peers}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValidationError as error:
        print(f"protocol verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
