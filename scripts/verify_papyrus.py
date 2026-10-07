#!/usr/bin/env python3
"""Audit original Papyrus native declarations against C++ bindings; this is not a PEX compiler."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def verify(source: str, native: str) -> int:
    lines = [line.split(";", 1)[0].strip() for line in source.splitlines()]
    lines = [line for line in lines if line]
    if not lines or lines.pop(0) != "Scriptname SYNTHNative Extends ScriptObject Hidden":
        raise ValueError("unexpected Papyrus script identity")
    declared = {}
    for line in lines:
        match = re.fullmatch(r"(Bool|Int) Function (\w+)\(([^)]*)\) Global Native", line)
        if not match or match[2] in declared:
            raise ValueError(f"unsupported or duplicate native declaration: {line}")
        arguments = []
        for argument in match[3].split(",") if match[3] else []:
            parameter = re.fullmatch(r"\s*(Int|String) \w+\s*", argument)
            if not parameter:
                raise ValueError(f"invalid native argument: {argument}")
            arguments.append(parameter[1])
        declared[match[2]] = (match[1], arguments)
    types = {"bool": "Bool", "std::int32_t": "Int", "std::string_view": "String"}
    callbacks = {}
    for result, name, parameters in re.findall(r"inline (\S+) (papyrus_\w+)\(([^)]*)\)", native):
        parameters = [parameter.strip().split()[0] for parameter in parameters.split(",")]
        if parameters.pop(0) != "std::monostate":
            raise ValueError("Papyrus entry is not global")
        callbacks[name] = (types[result], [types[parameter] for parameter in parameters])
    bindings = re.findall(r'bind\("(\w+)", (papyrus_\w+)\)', native)
    if len(bindings) != len(dict(bindings)):
        raise ValueError("duplicate native registration")
    registered = {name: callbacks[callback] for name, callback in bindings}
    expected = {"IsAvailable", "SpeakExact", "Comment", "React", "Ask", "OpenPrompt"}
    if set(declared) != expected or declared != registered:
        raise ValueError(f"Papyrus/native contract mismatch: {declared} != {registered}")
    return len(declared)


if __name__ == "__main__":
    count = verify((ROOT / "scripts/papyrus/SYNTHNative.psc").read_text(encoding="utf-8"),
                   (ROOT / "src/adapters/commonlib_papyrus_api.hpp").read_text(encoding="utf-8"))
    print(f"Papyrus source/native contract passed: {count} declarations; source audit only, PEX build is separate")
