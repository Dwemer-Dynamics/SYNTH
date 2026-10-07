#pragma once
#include "core/save_context.hpp"
#include "core/quest_event_mailbox.hpp"
#include "core/player_event_sampler.hpp"
#include "client/actor_event_binding.hpp"

#include "json/json.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace synth::protocol_native {

struct Identity { std::string form_id, origin_plugin, playthrough_id, display_name; };
struct InventoryItem {
    std::string form_id, origin_plugin, display_name;
    std::uint64_t count{}, form_type{};
    std::int64_t value{};
    double weight{};
    bool equipped{};
};
struct FactionMembership {
    std::string form_id, origin_plugin, display_name, editor_id;
    std::int64_t rank{};
};
struct PackageState {
    bool available{};
    std::string form_id, origin_plugin, display_name, editor_id;
};
struct ActorState {
    Identity actor;
    std::string base_form_id, base_origin_plugin;
    double x{}, y{}, z{}, distance{};
    bool alive{}, disabled{}, in_combat{}, hostile_to_player{}, sneaking{};
    std::uint64_t level{};
    std::string race, sex, voice_type;
    bool teammate{};
    double health_percent{}, action_points_percent{};
    std::vector<InventoryItem> inventory;
    std::string line_of_sight{"unknown"};
    std::vector<FactionMembership> factions;
    std::string faction_observation{"unavailable"};
    std::string life_state{"unknown"}, posture{"unknown"};
    bool weapon_drawn{};
    double movement_speed{};
    bool sprinting{}, talking_to_player{}, in_power_armor{};
    PackageState current_package;
    std::string inventory_observation{"unavailable"};
    std::string faction_completeness;
    bool health_percent_available{}, action_points_percent_available{};
};
struct WorldState {
    std::string location, cell, worldspace, weather;
    bool interior{};
    std::uint64_t game_time_ticks{};
    std::optional<core::SceneIdentity> scene;
};
struct LoadedPlugin {
    std::string plugin_name, form_id_prefix;
    bool is_light{};
    std::uint16_t compile_index{}, small_file_compile_index{}, partial_index{};
};
struct QuestState {
    std::string form_id, origin_plugin, display_name, editor_id;
    std::uint64_t current_stage{}, active_objectives{};
    std::vector<std::string> objectives;
    std::string objectives_observation{"partial"};
    std::optional<bool> tracked;
};
struct NativeQuestTransition {
    core::QuestEvent event;
    std::string origin_plugin;
    std::uint64_t visibility_sequence{};
    bool operator==(const NativeQuestTransition&) const = default;
};
struct NativeQuestBatch {
    std::string batch_id;
    std::vector<NativeQuestTransition> events;
    bool operator==(const NativeQuestBatch&) const = default;
};
struct NativePlayerEvent {
    core::PlayerEventTransition transition;
    std::uint64_t age_ms{};
};
struct NativeActorBatch {
    std::uint64_t serial{}, age_ms{};
    std::shared_ptr<const client::BoundActorEventScene> capture;
};
struct NearbyItemState {
    std::string reference_id, base_form_id, cell_form_id;
    std::optional<std::string> reference_origin_plugin;
    std::string base_origin_plugin, cell_origin_plugin, display_name;
    double x{}, y{}, z{}, distance{}, weight{};
    std::uint64_t count{}, form_type{};
    std::int64_t value{};
    bool stealing{}, looking_at{}, held{};
};
struct PointOfInterestState {
    std::string reference_id, base_form_id, cell_form_id;
    std::optional<std::string> reference_origin_plugin;
    std::string base_origin_plugin, cell_origin_plugin, display_name, kind;
    double x{}, y{}, z{}, distance{};
    bool locked{}, looking_at{};
};
struct Narrator { std::string id, display_name; };
using Speaker = std::variant<Identity, Narrator>;
struct Media { std::string media_id, content_type, sha256; std::uint64_t size{}; };
struct Speech {
    std::string utterance_id, cache_key, status;
    std::uint64_t deadline_ms{};
    std::optional<Media> media;
};
struct TtsStatus {
    std::string cache_key, status, error_code;
    std::optional<Media> media;
};
struct VisualResponse {
    std::string request_id, turn_id, capture_id, status, detail;
    std::uint64_t generation{};
    bool ok{};
};
struct Action {
    std::string action_id, idempotency_key, name, capability;
    Identity actor;
    std::optional<Identity> target;
    json::Value arguments;
    std::uint64_t deadline_ms{};
};
struct Start { std::string server_version; };
struct Dialogue {
    Speaker speaker;
    std::string text;
    std::optional<Media> media;
    std::optional<Speech> speech;
    std::optional<Identity> listener;
};
struct Status { std::string code, detail; bool rechat_scene{}; };
struct End { std::string status; };
using Payload = std::variant<Start, Dialogue, Action, Status, End>;
struct Line {
    std::string line_id, request_id, turn_id, runtime_variant;
    std::uint64_t generation{};
    Payload payload;
    std::uint64_t protocol_version{1};
    std::uint64_t context_sequence{};
};

inline const json::Value& required(const json::Value& object, std::string_view key) {
    const auto* value = object.find(key);
    if (!value) throw std::invalid_argument{"missing protocol field: " + std::string{key}};
    return *value;
}
inline void closed_object(const json::Value& value,
                          std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) throw std::invalid_argument{"protocol value is not an object"};
    for (const auto& [key, ignored] : value.as_object()) {
        (void)ignored;
        if (std::ranges::find(allowed, std::string_view{key}) == allowed.end())
            throw std::invalid_argument{"unknown protocol field: " + key};
    }
}
inline std::string text(const json::Value& object, std::string_view key) {
    const auto& value = required(object, key);
    if (!value.is_string()) throw std::invalid_argument{"protocol field is not text"};
    return value.as_string();
}
inline std::uint64_t integer(const json::Value& object, std::string_view key) {
    const auto& value = required(object, key);
    if (const auto* item = std::get_if<std::uint64_t>(&value.storage())) return *item;
    if (const auto* item = std::get_if<std::int64_t>(&value.storage()); item && *item >= 0) return static_cast<std::uint64_t>(*item);
    throw std::invalid_argument{"protocol field is not an unsigned integer"};
}
inline Identity identity(const json::Value& value) {
    closed_object(value, {"form_id", "origin_plugin", "playthrough_id", "display_name"});
    Identity out{text(value, "form_id"), text(value, "origin_plugin"), text(value, "playthrough_id"),
                 value.find("display_name") ? text(value, "display_name") : ""};
    if (out.form_id.size() != 10 || !out.form_id.starts_with("0x") || out.origin_plugin.find_first_of("/\\") != std::string::npos || out.playthrough_id.empty())
        throw std::invalid_argument{"invalid protocol identity"};
    return out;
}
inline Media media(const json::Value& value) {
    closed_object(value, {"schema", "media_id", "content_type", "sha256", "size"});
    if (text(value, "schema") != "synth.media.v1")
        throw std::invalid_argument{"invalid media schema"};
    Media out{text(value,"media_id"), text(value,"content_type"), text(value,"sha256"), integer(value,"size")};
    if (out.media_id != "media/" + out.sha256 || out.sha256.size()!=64 || out.size==0 ||
        out.size > 16U * 1024U * 1024U) throw std::invalid_argument{"invalid media reference"};
    return out;
}
inline Speech speech(const json::Value& value) {
    closed_object(value, {"schema", "utterance_id", "cache_key", "status", "deadline_ms", "media"});
    if (text(value, "schema") != "synth.speech.v1")
        throw std::invalid_argument{"invalid speech schema"};
    Speech out{text(value, "utterance_id"), text(value, "cache_key"), text(value, "status"),
               integer(value, "deadline_ms"), std::nullopt};
    if (out.utterance_id.empty() || out.utterance_id.size() > 128 || out.cache_key.size() != 32 ||
        (out.status != "pending" && out.status != "ready" && out.status != "failed") ||
        out.deadline_ms == 0 || out.deadline_ms > 120000)
        throw std::invalid_argument{"invalid speech job"};
    if (const auto* value_media = value.find("media")) out.media = media(*value_media);
    if (out.status == "ready" && !out.media)
        throw std::invalid_argument{"ready speech job has no media"};
    return out;
}
inline TtsStatus decode_tts_status(std::string_view wire) {
    const auto root = json::parse(wire);
    if (!root.is_object()) throw std::invalid_argument{"TTS status is not an object"};
    for (const auto& [key, ignored] : root.as_object()) {
        (void)ignored;
        if (key != "schema" && key != "cache_key" && key != "status" &&
            key != "updated_at" && key != "error_code" && key != "media")
            throw std::invalid_argument{"unknown TTS status field"};
    }
    if (text(root, "schema") != "synth.tts_status.v1")
        throw std::invalid_argument{"invalid TTS status schema"};
    TtsStatus result{text(root, "cache_key"), text(root, "status"), {}, std::nullopt};
    if (result.cache_key.size() != 32 ||
        (result.status != "pending" && result.status != "ready" &&
         result.status != "failed" && result.status != "unknown"))
        throw std::invalid_argument{"invalid TTS status"};
    if (const auto* code = root.find("error_code")) {
        if (!code->is_string() || code->as_string().size() > 64)
            throw std::invalid_argument{"invalid TTS error code"};
        result.error_code = code->as_string();
    }
    if (const auto* value_media = root.find("media")) result.media = media(*value_media);
    if (result.status == "ready" && !result.media)
        throw std::invalid_argument{"ready TTS status has no media"};
    return result;
}
inline Speaker speaker(const json::Value& value) {
    if (value.find("form_id")) return identity(value);
    closed_object(value, {"kind", "id", "display_name"});
    if (text(value, "kind") != "narrator" || text(value, "id") != "narrator")
        throw std::invalid_argument{"invalid protocol speaker"};
    return Narrator{text(value, "id"), text(value, "display_name")};
}
inline Line decode_line(std::string_view wire) {
    auto root = json::parse(wire);
    const auto version = integer(root, "protocol_version");
    if (version == 2) {
        closed_object(root, {"schema", "protocol_version", "type", "line_id", "request_id", "turn_id",
                             "generation", "game", "runtime_variant", "payload", "context_sequence"});
    } else {
        closed_object(root, {"schema", "protocol_version", "type", "line_id", "request_id", "turn_id",
                             "generation", "game", "runtime_variant", "payload"});
    }
    if ((version != 1 && version != 2) || text(root,"schema") != "synth.response.line.v" + std::to_string(version) || text(root,"game")!="fo4")
        throw std::invalid_argument{"unsupported response line"};
    Line line{text(root,"line_id"), text(root,"request_id"), text(root,"turn_id"), text(root,"runtime_variant"), integer(root,"generation"), Start{}};
    line.protocol_version = version;
    line.context_sequence = version == 2 ? integer(root, "context_sequence") : 0;
    if (line.context_sequence > 9'007'199'254'740'991ULL)
        throw std::invalid_argument{"response context sequence is out of bounds"};
    if (line.runtime_variant != "flat" && line.runtime_variant != "vr")
        throw std::invalid_argument{"invalid response runtime lane"};
    const auto type=text(root,"type"); const auto& payload=required(root,"payload");
    if(type=="response_start") {
        closed_object(payload, {"server_version"});
        line.payload=Start{text(payload,"server_version")};
    }
    else if(type=="dialogue") {
        if (version == 2) closed_object(payload, {"speaker", "text", "media", "speech", "listener"});
        else closed_object(payload, {"speaker", "text", "media", "speech"});
        Dialogue d{speaker(required(payload,"speaker")), text(payload,"text"), std::nullopt,
                   std::nullopt};
        if(auto* m=payload.find("media")) d.media=media(*m);
        if(auto* s=payload.find("speech")) d.speech=speech(*s);
        if (const auto* listener=payload.find("listener")) {
            d.listener=identity(*listener);
            const auto* actor=std::get_if<Identity>(&d.speaker);
            std::uint32_t form{};
            const auto& id=d.listener->form_id;
            const auto [end,error]=std::from_chars(id.data()+2,id.data()+id.size(),form,16);
            const auto equal_folded=[](std::string_view left,std::string_view right) {
                const auto lower=[](unsigned char ch) { return ch>='A' && ch<='Z' ? ch+('a'-'A') : ch; };
                return std::ranges::equal(left,right,[&](auto a,auto b) { return lower(a)==lower(b); });
            };
            const auto& plugin=d.listener->origin_plugin;
            const auto& save=d.listener->playthrough_id;
            const auto alphanumeric=[](unsigned char ch) {
                return (ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || (ch>='0' && ch<='9');
            };
            const bool valid_plugin=plugin.size()>4 && plugin.size()<=255 &&
                (equal_folded(std::string_view{plugin}.substr(plugin.size()-4),".esm") ||
                 equal_folded(std::string_view{plugin}.substr(plugin.size()-4),".esp") ||
                 equal_folded(std::string_view{plugin}.substr(plugin.size()-4),".esl")) &&
                std::ranges::none_of(plugin,[](unsigned char ch) { return ch<0x20 || ch==0x7f; });
            if (!actor || line.context_sequence==0 || error!=std::errc{} || end!=id.data()+id.size() || form==0
                || !valid_plugin || save.size()>128 || !alphanumeric(save.front())
                || !std::ranges::all_of(save,[&](unsigned char ch) { return alphanumeric(ch) || ch=='.' || ch=='_' || ch==':' || ch=='-'; })
                || d.listener->display_name.size()>255 || (listener->find("display_name") && d.listener->display_name.empty())
                || d.listener->playthrough_id!=actor->playthrough_id
                || (equal_folded(id,actor->form_id) && equal_folded(d.listener->origin_plugin,actor->origin_plugin)))
                throw std::invalid_argument{"invalid canonical dialogue listener"};
        }
        line.payload=std::move(d);
    }
    else if(type=="action") {
        closed_object(payload, {"action"});
        const auto& a=required(payload,"action");
        closed_object(a, {"schema", "action_id", "idempotency_key", "name", "capability", "actor",
                          "target", "arguments", "deadline_ms"});
        if (text(a, "schema") != "synth.action.v1")
            throw std::invalid_argument{"invalid action schema"};
        std::optional<Identity> target;
        if (const auto* value=a.find("target")) target=identity(*value);
        const auto& arguments=required(a,"arguments");
        if (!arguments.is_object()) throw std::invalid_argument{"action arguments are not an object"};
        closed_object(arguments, {"target_name", "item", "amount", "location", "message", "instruction", "speed"});
        line.payload=Action{text(a,"action_id"),text(a,"idempotency_key"),text(a,"name"),
                            text(a,"capability"),identity(required(a,"actor")),std::move(target),
                            arguments,integer(a,"deadline_ms")};
    }
    else if(type=="status") {
        if (line.protocol_version == 2) closed_object(payload, {"code", "detail", "rechat_scene"});
        else closed_object(payload, {"code", "detail"});
        line.payload=Status{text(payload,"code"), payload.find("detail")?text(payload,"detail"):"",
            payload.find("rechat_scene") ? required(payload,"rechat_scene").as_bool() : false};
    }
    else if(type=="response_end") {
        closed_object(payload, {"status"});
        const auto status = text(payload,"status");
        if (status != "complete" && status != "cancelled" && status != "error")
            throw std::invalid_argument{"invalid response terminal status"};
        line.payload=End{status};
    }
    else throw std::invalid_argument{"unknown response line type"};
    return line;
}

inline VisualResponse decode_visual_response(std::string_view wire) {
    const auto root = json::parse(wire);
    closed_object(root, {"schema", "request_id", "turn_id", "generation", "capture_id", "ok",
                         "status", "record_id", "description", "provider", "model", "portrait_media",
                         "actor", "code", "error", "retryable", "replayed"});
    if (text(root, "schema") != "synth.visual_context.response.v1")
        throw std::invalid_argument{"invalid visual response schema"};
    const auto& ok_value = required(root, "ok");
    if (!ok_value.is_bool()) throw std::invalid_argument{"visual response ok is not boolean"};
    VisualResponse result{text(root, "request_id"), text(root, "turn_id"), text(root, "capture_id"),
                          text(root, "status"), {}, integer(root, "generation"), ok_value.as_bool()};
    if ((result.ok && result.status != "success") || (!result.ok && result.status != "error"))
        throw std::invalid_argument{"visual response status does not match ok"};
    if (const auto* record_id = root.find("record_id")) {
        if (integer(root, "record_id") == 0)
            throw std::invalid_argument{"visual record ID is invalid"};
        (void)record_id;
    }
    for (const auto key : {"provider", "model", "code"}) {
        if (root.find(key)) (void)text(root, key);
    }
    for (const auto key : {"retryable", "replayed"}) {
        if (const auto* value = root.find(key); value && !value->is_bool())
            throw std::invalid_argument{"visual response flag is not boolean"};
    }
    if (const auto* error = root.find("error")) {
        if (!error->is_string()) throw std::invalid_argument{"visual error is not text"};
        result.detail = error->as_string();
    } else if (const auto* description = root.find("description")) {
        if (!description->is_string()) throw std::invalid_argument{"visual description is not text"};
        result.detail = description->as_string();
    }
    if (const auto* portrait = root.find("portrait_media")) (void)media(*portrait);
    if (const auto* actor = root.find("actor")) (void)identity(*actor);
    return result;
}

struct ActionInventoryObservation {
    Identity actor;
    std::vector<InventoryItem> items;
    std::string observation{"unavailable"};
};
struct ActionTransferObservation {
    ActionInventoryObservation donor, recipient;
};
struct ActionResultEvent {
    std::string session_id, request_id, turn_id, runtime_variant, client_version, runtime_version;
    std::uint64_t generation{};
    std::vector<std::string> capabilities;
    std::string action_id, idempotency_key, status, detail;
    std::uint64_t protocol_version{1};
    std::uint64_t context_sequence{};
    std::optional<ActionInventoryObservation> inventory;
    std::optional<ActionTransferObservation> transfer;
};
inline json::Value::Object object(std::initializer_list<std::pair<std::string,json::Value>> items){ return {items}; }
inline json::Value identity_value(const Identity& value) {
    auto result = object({{"form_id", value.form_id}, {"origin_plugin", value.origin_plugin},
                          {"playthrough_id", value.playthrough_id}});
    if (!value.display_name.empty()) result.emplace_back("display_name", value.display_name);
    return json::Value{std::move(result)};
}
inline json::Value media_value(const Media& value) {
    return json::Value{object({{"schema", "synth.media.v1"}, {"media_id", value.media_id},
                               {"content_type", value.content_type}, {"sha256", value.sha256},
                               {"size", value.size}})};
}
// Keep observation quality and truncation truthful within the action-result text budget.
inline std::pair<std::string, std::string> inventory_action_result(
    const Identity& actor, json::Value::Array items, std::string_view observation) {
    if (observation != "complete" && observation != "partial") {
        return {"unavailable", "inventory was not observed in the captured frame; contents are unknown"};
    }
    bool truncated = observation == "partial";
    if (items.size() > 32) { items.resize(32); truncated = true; }
    for (;;) {
        auto detail = json::write(json::Value{object({
            {"schema", "synth.action.check_inventory.result.v1"},
            {"actor", identity_value(actor)}, {"items", items},
            {"inventory_observation", truncated ? "partial" : "complete"},
            {"truncated", truncated},
        })});
        if (detail.size() <= 1024) return {"succeeded", std::move(detail)};
        if (items.empty()) return {"unavailable", "inventory result identity exceeds response limit"};
        items.pop_back();
        truncated = true;
    }
}
inline json::Value actor_state_value(const ActorState& value) {
    json::Value::Array inventory;
    inventory.reserve(value.inventory.size());
    for (const auto& item : value.inventory) {
        inventory.emplace_back(json::Value{object({
            {"form_id", item.form_id}, {"origin_plugin", item.origin_plugin},
            {"display_name", item.display_name}, {"count", item.count},
            {"value", item.value}, {"weight", item.weight},
            {"form_type", item.form_type}, {"equipped", item.equipped},
        })});
    }
    json::Value::Array factions;
    factions.reserve(value.factions.size());
    for (const auto& faction : value.factions) {
        factions.emplace_back(json::Value{object({
            {"form_id", faction.form_id}, {"origin_plugin", faction.origin_plugin},
            {"display_name", faction.display_name}, {"editor_id", faction.editor_id},
            {"rank", faction.rank},
        })});
    }
    return json::Value{object({
        {"actor", identity_value(value.actor)},
        {"base_form_id", value.base_form_id}, {"base_origin_plugin", value.base_origin_plugin},
        {"position", json::Value{object({{"x", value.x}, {"y", value.y}, {"z", value.z}})}},
        {"distance", value.distance}, {"alive", value.alive}, {"disabled", value.disabled},
        {"in_combat", value.in_combat}, {"hostile_to_player", value.hostile_to_player},
        {"sneaking", value.sneaking}, {"level", value.level},
        {"race", value.race}, {"sex", value.sex}, {"voice_type", value.voice_type},
        {"teammate", value.teammate}, {"health_percent", value.health_percent},
        {"action_points_percent", value.action_points_percent},
        {"health_percent_available", value.health_percent_available},
        {"action_points_percent_available", value.action_points_percent_available},
        {"inventory", std::move(inventory)},
        {"inventory_observation", value.inventory_observation},
        {"line_of_sight", value.line_of_sight},
        {"factions", std::move(factions)},
        {"faction_observation", value.faction_observation},
        {"faction_completeness", value.faction_completeness.empty()
            ? (value.faction_observation == "unavailable" ? "unavailable" : "partial")
            : value.faction_completeness},
        {"life_state", value.life_state}, {"posture", value.posture},
        {"weapon_drawn", value.weapon_drawn}, {"movement_speed", value.movement_speed},
        {"sprinting", value.sprinting}, {"talking_to_player", value.talking_to_player},
        {"in_power_armor", value.in_power_armor},
        {"current_package", json::Value{object({
            {"available", value.current_package.available},
            {"form_id", value.current_package.form_id},
            {"origin_plugin", value.current_package.origin_plugin},
            {"display_name", value.current_package.display_name},
            {"editor_id", value.current_package.editor_id},
        })}},
    })};
}
inline json::Value world_state_value(const WorldState& value, bool include_scene = false) {
    auto result = object({
        {"location", value.location}, {"cell", value.cell},
        {"worldspace", value.worldspace}, {"weather", value.weather},
        {"interior", value.interior}, {"game_time_ticks", value.game_time_ticks},
    });
    if (include_scene && value.scene) {
        const auto& scene = *value.scene;
        if (!scene.valid(value.interior)) throw std::invalid_argument{"invalid physical scene identity"};
        const auto reference = [](std::uint32_t id, const std::string& plugin) {
            char form[11]{};
            std::snprintf(form, sizeof(form), "0x%08X", id);
            return json::Value{object({{"form_id", std::string{form}}, {"origin_plugin", plugin}})};
        };
        result.emplace_back("scene", json::Value{object({
            {"cell", reference(scene.cell_form_id, scene.cell_origin_plugin)},
            {"worldspace", value.interior ? json::Value{nullptr}
                : reference(scene.worldspace_form_id, scene.worldspace_origin_plugin)}})});
    }
    return json::Value{std::move(result)};
}
inline json::Value quest_state_value(const QuestState& value, bool include_tracking = false) {
    json::Value::Array objectives;
    for (const auto& objective : value.objectives) objectives.emplace_back(objective);
    auto result = object({
        {"form_id", value.form_id}, {"origin_plugin", value.origin_plugin},
        {"display_name", value.display_name}, {"editor_id", value.editor_id},
        {"current_stage", value.current_stage},
        {"active_objectives", value.active_objectives},
        {"objectives", std::move(objectives)},
        {"objectives_observation", value.objectives_observation},
    });
    if (include_tracking && value.tracked) result.emplace_back("tracked", *value.tracked);
    return json::Value{std::move(result)};
}
// A fresh partial list can be useful, but cached or unobserved quests cannot satisfy a fresh read.
inline std::pair<std::string, std::string> quest_action_result(
    json::Value::Array quests, std::string_view observation) {
    if (observation != "complete" && observation != "partial")
        return {"unavailable", "active quests were not freshly observed; current quest state is unknown"};
    bool truncated = observation == "partial";
    if (quests.size() > 16) { quests.resize(16); truncated = true; }
    for (;;) {
        auto detail = json::write(json::Value{object({
            {"schema", "synth.action.read_quests.result.v1"}, {"quests", quests},
            {"active_quests_observation", truncated ? "partial" : "complete"}, {"truncated", truncated},
        })});
        if (detail.size() <= 1024) return {"succeeded", std::move(detail)};
        quests.pop_back();
        truncated = true;
    }
}
// Serialization limits can reduce completeness, never upgrade freshness or unavailable evidence.
inline std::string retained_collection_observation(std::string_view quality, std::size_t observed, std::size_t retained) {
    return quality == "complete" && retained < observed ? "partial" : std::string{quality};
}
inline json::Value nearby_item_value(const NearbyItemState& value) {
    return json::Value{object({
        {"reference_id", value.reference_id}, {"base_form_id", value.base_form_id},
        {"cell_form_id", value.cell_form_id},
        {"reference_origin_plugin", value.reference_origin_plugin
                                            ? json::Value{*value.reference_origin_plugin}
                                            : json::Value{""}},
        {"base_origin_plugin", value.base_origin_plugin},
        {"cell_origin_plugin", value.cell_origin_plugin},
        {"display_name", value.display_name},
        {"position", json::Value{object({{"x", value.x}, {"y", value.y}, {"z", value.z}})}},
        {"distance", value.distance}, {"count", value.count}, {"value", value.value},
        {"weight", value.weight}, {"form_type", value.form_type},
        {"stealing", value.stealing}, {"looking_at", value.looking_at}, {"held", value.held},
    })};
}
inline json::Value point_of_interest_value(const PointOfInterestState& value) {
    return json::Value{object({
        {"reference_id", value.reference_id}, {"base_form_id", value.base_form_id},
        {"cell_form_id", value.cell_form_id},
        {"reference_origin_plugin", value.reference_origin_plugin
                                            ? json::Value{*value.reference_origin_plugin}
                                            : json::Value{""}},
        {"base_origin_plugin", value.base_origin_plugin},
        {"cell_origin_plugin", value.cell_origin_plugin},
        {"display_name", value.display_name}, {"kind", value.kind},
        {"position", json::Value{object({{"x", value.x}, {"y", value.y}, {"z", value.z}})}},
        {"distance", value.distance}, {"locked", value.locked},
        {"looking_at", value.looking_at},
    })};
}
inline json::Value loaded_plugin_value(const LoadedPlugin& value) {
    return json::Value{object({
        {"plugin_name", value.plugin_name}, {"is_light", value.is_light},
        {"compile_index", static_cast<std::uint64_t>(value.compile_index)},
        {"small_file_compile_index", static_cast<std::uint64_t>(value.small_file_compile_index)},
        {"partial_index", static_cast<std::uint64_t>(value.partial_index)},
        {"formid_prefix", value.form_id_prefix},
    })};
}
struct EventContext {
    std::string session_id, request_id, turn_id, runtime_variant, client_version, runtime_version;
    std::uint64_t generation{};
    std::vector<std::string> capabilities;
    std::uint64_t protocol_version{1};
    std::uint64_t context_sequence{};
};
inline std::string encode_event(const EventContext& context, std::string type, json::Value payload) {
    if (context.protocol_version != 1 && context.protocol_version != 2)
        throw std::invalid_argument{"unsupported event protocol version"};
    if (context.protocol_version == 2) {
        const bool bound = type == "input_text" || type == "input_audio" || type == "activate"
            || type == "trigger" || type == "profile_refresh" || type == "diary_request" || type == "action_result";
        if (context.context_sequence > 9'007'199'254'740'991ULL
            || (bound ? context.context_sequence == 0 : context.context_sequence != 0))
            throw std::invalid_argument{"event context binding is invalid"};
    }
    json::Value::Array caps; for(const auto& capability:context.capabilities) caps.emplace_back(capability);
    auto fields=object({{"schema","synth.event.v" + std::to_string(context.protocol_version)},{"protocol_version",context.protocol_version},{"type",std::move(type)},
        {"session_id",context.session_id},{"request_id",context.request_id},{"turn_id",context.turn_id},{"generation",context.generation},
        {"game","fo4"},{"runtime_variant",context.runtime_variant},{"client_version",context.client_version},
        {"runtime_version",context.runtime_version},{"capabilities",std::move(caps)},{"payload",std::move(payload)}});
    if (context.protocol_version == 2) fields.emplace_back("context_sequence", context.context_sequence);
    return json::write(json::Value{std::move(fields)})+"\n";
}
inline std::string encode_init(const EventContext& context, std::string locale, bool hmd_pose_fresh=false,
                               const std::vector<LoadedPlugin>& loaded_plugins = {}) {
    auto payload=object({{"locale",std::move(locale)}});
    if(context.runtime_variant=="vr") payload.emplace_back("hmd_pose_fresh",hmd_pose_fresh);
    if(!loaded_plugins.empty()) {
        json::Value::Array plugins;
        for(const auto& plugin:loaded_plugins) plugins.emplace_back(loaded_plugin_value(plugin));
        payload.emplace_back("plugins",std::move(plugins));
    }
    return encode_event(context,"init",json::Value{std::move(payload)});
}
struct ControlSelection final {
    std::string mode;
    std::uint64_t model_slot{1};
};

inline bool valid_control_mode(std::string_view mode) {
    constexpr std::string_view modes[]{"STANDARD", "WHISPER", "CLOSE", "SHOUT", "NARRATOR", "DIRECTOR", "INJECTION_LOG", "INJECTION_CHAT", "CHEATMODE"};
    return std::ranges::find(modes, mode) != std::end(modes);
}

// Optional negotiated settings are copied at turn admission, never read from a later menu selection.
inline void append_control_selection(json::Value::Object& payload, const EventContext& context, const ControlSelection& selection) {
    if (selection.mode.empty()) return;
    if (context.protocol_version != 2 || !valid_control_mode(selection.mode) || selection.model_slot < 1 || selection.model_slot > 4 ||
        std::ranges::find(context.capabilities, "control.menu") == context.capabilities.end())
        throw std::invalid_argument{"invalid or unnegotiated control selection"};
    payload.emplace_back("control", json::Value{object({{"mode", selection.mode}, {"model_slot", selection.model_slot}})});
}

inline std::string encode_control(const EventContext& context, std::string setting, std::string value) {
    const auto valid = (setting == "status" && value.empty()) || (setting == "mode" && valid_control_mode(value)) ||
        (setting == "model_slot" && (value == "1" || value == "2" || value == "3" || value == "4"));
    if (!valid || context.protocol_version != 2 || context.context_sequence != 0 ||
        std::ranges::find(context.capabilities, "control.menu") == context.capabilities.end())
        throw std::invalid_argument{"invalid or unnegotiated control request"};
    return encode_event(context, "control", json::Value{object({{"setting", std::move(setting)}, {"value", std::move(value)}})});
}

inline std::string encode_input_text(const EventContext& context, std::string text_value, const ControlSelection& selection = {}) {
    auto payload = object({{"text",std::move(text_value)}});
    append_control_selection(payload, context, selection);
    return encode_event(context,"input_text",json::Value{std::move(payload)});
}
inline std::string encode_input_audio(const EventContext& context, const Media& value, const ControlSelection& selection = {}) {
    auto encoded_media = media_value(value);
    auto payload = object({{"media",std::move(encoded_media)}});
    append_control_selection(payload, context, selection);
    return encode_event(context,"input_audio",json::Value{std::move(payload)});
}

inline std::string encode_visual_capture(
    const EventContext& context,
    std::string capture_id,
    std::string interaction_mode,
    std::string perspective,
    const Media& media,
    const std::optional<WorldState>& world,
    const std::optional<Identity>& subject,
    const std::vector<Identity>& nearby_actors) {
    if (context.protocol_version == 2 && context.context_sequence == 0)
        throw std::invalid_argument{"visual capture requires an acknowledged snapshot"};
    json::Value::Array nearby;
    nearby.reserve(nearby_actors.size());
    for (const auto& actor : nearby_actors) nearby.emplace_back(identity_value(actor));
    auto world_value = json::Value{object({
        {"location", world ? world->location : ""},
        {"worldspace", world ? world->worldspace : ""},
        {"cell_form_id", ""},
        {"worldspace_form_id", ""},
        {"weather", world ? world->weather : ""},
        {"interior", world ? world->interior : false},
        {"game_time_ticks", world ? world->game_time_ticks : std::uint64_t{0}},
    })};
    auto root = object({
        {"schema", context.protocol_version == 2 ? "synth.visual_context.capture.v2" : "synth.visual_context.capture.v1"},
        {"protocol_version", context.protocol_version},
        {"session_id", context.session_id},
        {"request_id", context.request_id},
        {"turn_id", context.turn_id},
        {"generation", context.generation},
        {"game", "fo4"},
        {"runtime_variant", context.runtime_variant},
        {"capture_id", std::move(capture_id)},
        {"interaction_mode", std::move(interaction_mode)},
        {"perspective", std::move(perspective)},
        {"media", media_value(media)},
        {"world", std::move(world_value)},
        {"nearby_actors", json::Value{std::move(nearby)}},
        {"visual_type", subject ? "actor" : "scene"},
        {"visual_key", ""},
    });
    if (subject) root.emplace_back("subject", identity_value(*subject));
    if (context.protocol_version == 2) root.emplace_back("context_sequence", context.context_sequence);
    return json::write(json::Value{std::move(root)}) + "\n";
}
// Bounded, exact native scalars; journal visibility is checked against the owned server context.
inline json::Value quest_events_value(const NativeQuestBatch& batch, std::uint64_t sequence) {
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (batch.batch_id.empty() || batch.batch_id.size() > 128 || !alnum(batch.batch_id.front()) ||
        !std::ranges::all_of(batch.batch_id, [&](char c) { return alnum(c) || c == '.' || c == '_' || c == ':' || c == '-'; }) ||
        batch.events.empty() || batch.events.size() > core::QuestEventMailbox::capacity ||
        sequence == 0 || sequence > 9'007'199'254'740'991ULL)
        throw std::invalid_argument{"invalid native quest batch"};
    json::Value::Array events;
    events.reserve(batch.events.size());
    for (const auto& value : batch.events) {
        const auto& plugin = value.origin_plugin;
        if (!value.event.valid() || value.visibility_sequence >= sequence || plugin.size() <= 4 || plugin.size() > 255 ||
            plugin.find_first_of("/\\") != std::string::npos || plugin.find('\0') != std::string::npos ||
            (!plugin.ends_with(".esm") && !plugin.ends_with(".esp") && !plugin.ends_with(".esl")))
            throw std::invalid_argument{"invalid native quest transition"};
        char form_id[11]{};
        std::snprintf(form_id, sizeof(form_id), "0x%08X", value.event.form_id);
        const auto kind = value.event.kind == core::QuestEventKind::stage ? "stage" :
            value.event.kind == core::QuestEventKind::started ? "started" : "stopped";
        events.emplace_back(object({{"kind", kind}, {"form_id", form_id}, {"origin_plugin", plugin},
            {"visibility_sequence", value.visibility_sequence}, {"stage", std::uint64_t{value.event.stage}},
            {"item", std::uint64_t{value.event.item}}, {"failed", value.event.failed}}));
    }
    return json::Value{object({{"batch_id", batch.batch_id}, {"events", std::move(events)}})};
}

// Exact sampled evidence; current context supplies identity, not a fabricated historical scene.
inline json::Value player_event_value(const NativePlayerEvent& event, const EventContext& context, const Identity& player) {
    const auto& value = event.transition;
    const auto& before = value.before;
    const auto& after = value.after;
    char form_id[11]{};
    std::snprintf(form_id, sizeof(form_id), "0x%08X", after.form_id);
    const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(after.observed_at - before.observed_at).count();
    const auto limit = core::RuntimeGeneration::maximum_wire_value;
    if (context.protocol_version != 2 || context.runtime_variant != "flat" ||
        std::ranges::find(context.capabilities, "context.player_events") == context.capabilities.end() ||
        value.serial == 0 || value.serial > limit || event.age_ms > limit || interval < 1 ||
        static_cast<std::uint64_t>(interval) > limit || before.generation != after.generation ||
        after.generation.value() != context.generation || before.variant != after.variant || after.variant != core::RuntimeVariant::flat ||
        before.form_id != after.form_id || before.origin_plugin != after.origin_plugin || before.playthrough_id != after.playthrough_id ||
        player.form_id != form_id || player.origin_plugin != after.origin_plugin || player.playthrough_id != after.playthrough_id ||
        before.level < 1 || after.level < before.level || before.game_time_ticks == 0 ||
        after.game_time_ticks < before.game_time_ticks || after.game_time_ticks > limit ||
        (value.kind == core::PlayerEventKind::level_up ? after.level <= before.level :
         value.kind == core::PlayerEventKind::combat_end ? (!before.in_combat || after.in_combat) : true))
        throw std::invalid_argument{"invalid owned player transition"};
    const auto state = [](const core::PlayerEventSample& sample) {
        return json::Value{object({{"game_time_ticks", sample.game_time_ticks},
            {"level", static_cast<std::uint64_t>(sample.level)}, {"in_combat", sample.in_combat}})};
    };
    return json::Value{object({{"event_id", "player:" + std::to_string(value.serial)},
        {"kind", value.kind == core::PlayerEventKind::level_up ? "level_up" : "combat_end"},
        {"before", state(before)}, {"after", state(after)},
        {"interval_ms", static_cast<std::uint64_t>(interval)}, {"age_ms", event.age_ms}})};
}

// Encode immutable bound evidence; delivery age may advance, but original capture delays and identities cannot.
inline json::Value actor_events_value(const NativeActorBatch& batch,const EventContext& context,const Identity& player) {
    const auto limit=core::RuntimeGeneration::maximum_wire_value;
    if (!batch.capture || !batch.capture->scene || batch.serial==0 || batch.serial>limit || batch.age_ms>limit ||
        batch.capture->events.empty() || batch.capture->events.size()>core::ActorEventMailbox::capacity ||
        context.protocol_version!=2 || context.runtime_variant!="flat" || context.session_id!=batch.capture->session_id ||
        context.generation!=batch.capture->scene->generation().value() ||
        batch.capture->scene->variant()!=core::RuntimeVariant::flat ||
        std::ranges::find(context.capabilities,"context.actor_events")==context.capabilities.end())
        throw std::invalid_argument{"invalid owned actor batch"};
    const auto form=[](std::uint32_t id) { char value[11]{};std::snprintf(value,sizeof(value),"0x%08X",id);return std::string{value}; };
    const auto identity=[&](const context::ActorIdentity& actor) {
        return json::Value{object({{"form_id",form(actor.form().form_id())},{"origin_plugin",actor.form().plugin()},
            {"playthrough_id",actor.playthrough().value()}})};
    };
    const auto& owner=batch.capture->scene->player();
    if (player.form_id!=form(owner.form_id()) || player.origin_plugin!=owner.origin_plugin() || player.playthrough_id!=owner.playthrough_id())
        throw std::invalid_argument{"actor batch player mismatch"};
    json::Value::Array events;
    for (const auto& bound:batch.capture->events) {
        const auto& event=bound.evidence;
        const auto delay=std::chrono::duration_cast<std::chrono::milliseconds>(batch.capture->scene->captured_at()-event.observed_at).count();
        const bool death=event.kind==core::ActorEventKind::death;
        const bool self_equipment=!death && event.actor==owner.form_id();
        if (!event.valid() || event.observed_at==core::SnapshotClock::time_point{} ||
            event.observed_at>batch.capture->scene->captured_at() || delay<0 || delay>2000 ||
            bound.subject.form().form_id()!=event.actor || bound.subject.playthrough().value()!=owner.playthrough_id() ||
            (bound.player_knowledge!=client::ActorEventKnowledge::unavailable && bound.player_knowledge!=client::ActorEventKnowledge::player_equipment) ||
            (bound.player_knowledge==client::ActorEventKnowledge::player_equipment)!=self_equipment ||
            (bound.other && (bound.other->form().form_id()!=event.other_actor || bound.other->playthrough().value()!=owner.playthrough_id())) ||
            (death ? bound.item.has_value() : (!bound.item || !bound.item->valid() || bound.item->form_id!=event.base_object)))
            throw std::invalid_argument{"invalid bound actor event"};
        json::Value item{nullptr};
        if (bound.item) item=json::Value{object({{"form_id",form(bound.item->form_id)},
            {"origin_plugin",bound.item->origin_plugin},{"display_name",bound.item->name},{"form_type",std::uint64_t{bound.item->form_type}}})};
        events.emplace_back(object({{"kind",death?"death":event.kind==core::ActorEventKind::equipped?"equipped":"unequipped"},
            {"actor",identity(bound.subject)},{"other_form_id",event.other_actor?json::Value{form(event.other_actor)}:json::Value{nullptr}},
            {"other_actor",bound.other?identity(*bound.other):json::Value{nullptr}},{"item",std::move(item)},
            {"original_reference",event.original_reference?json::Value{form(event.original_reference)}:json::Value{nullptr}},
            {"unique_id",std::uint64_t{event.unique_id}},{"capture_delay_ms",static_cast<std::uint64_t>(delay)},
            {"player_knowledge",self_equipment?"player_equipment":"unavailable"}}));
    }
    return json::Value{object({{"batch_id","actor:"+std::to_string(batch.serial)},{"age_ms",batch.age_ms},{"events",std::move(events)}})};
}

inline std::string encode_context(const EventContext& context, std::string playthrough_id,
                                  std::uint64_t sequence, const Identity& player,
                                  const std::optional<Identity>& target,
                                  const std::vector<Identity>& audience,
                                  const std::vector<ActorState>& actor_states = {},
                                  const std::optional<WorldState>& world = std::nullopt,
                                  const std::vector<QuestState>& active_quests = {},
                                  const std::vector<NearbyItemState>& nearby_items = {},
                                  const std::vector<PointOfInterestState>& points_of_interest = {},
                                  const std::optional<core::SavedContext>& loaded_context = std::nullopt,
                                  std::string active_quests_observation = {},
                                  std::string nearby_items_observation = {},
                                  std::string points_of_interest_observation = {},
                                  std::string audience_observation = {},
                                  const std::optional<NativeQuestBatch>& quest_events = std::nullopt,
                                  const std::optional<NativePlayerEvent>& player_event = std::nullopt,
                                  const std::optional<NativeActorBatch>& actor_events = std::nullopt) {
    if (quest_events && (context.protocol_version != 2 || context.runtime_variant != "flat" ||
        std::ranges::find(context.capabilities, "context.quest_events") == context.capabilities.end()))
        throw std::invalid_argument{"native quest batches require negotiated v2 flat capability"};
    if (!audience_observation.empty() && audience_observation != "complete" && audience_observation != "partial" &&
        (audience_observation != "unavailable" || !audience.empty()))
        throw std::invalid_argument{"invalid audience observation"};
    if (loaded_context && (context.protocol_version != 2 || !core::valid_saved_context(*loaded_context)
        || loaded_context->playthrough_id != playthrough_id
        || std::ranges::find(context.capabilities, "context.saved_anchor") == context.capabilities.end()))
        throw std::invalid_argument{"invalid or unnegotiated loaded context"};
    json::Value::Array actors; for(const auto& actor:audience) actors.emplace_back(identity_value(actor));
    auto payload=object({{"playthrough_id",std::move(playthrough_id)},{"snapshot_sequence",sequence},
        {"player",identity_value(player)},{"audience",std::move(actors)}});
    if(target) payload.emplace_back("target",identity_value(*target));
    if (quest_events) payload.emplace_back("quest_events", quest_events_value(*quest_events, sequence));
    if (actor_events) payload.emplace_back("actor_events",actor_events_value(*actor_events,context,player));
    if (player_event) payload.emplace_back("player_event", player_event_value(*player_event, context, player));
    if (loaded_context) payload.emplace_back("loaded_context", json::Value{object({
        {"playthrough_id", loaded_context->playthrough_id}, {"session_id", loaded_context->session_id},
        {"generation", loaded_context->generation}, {"context_sequence", loaded_context->context_sequence}})});
    if(!actor_states.empty()) {
        if (actor_states.size() > 16) throw std::invalid_argument{"too many context actor states"};
        json::Value::Array states;
        const std::size_t inventory_limit = context.protocol_version == 2 && context.runtime_variant == "flat" &&
            std::ranges::find(context.capabilities, "context.inventory_512") != context.capabilities.end() ? 512 : 32;
        for(const auto& state:actor_states) {
            if (state.inventory.size() > inventory_limit)
                throw std::invalid_argument{"inventory exceeds context capability limit"};
            states.emplace_back(actor_state_value(state));
        }
        payload.emplace_back("actor_states",std::move(states));
    }
    if(world) payload.emplace_back("world",world_state_value(*world, context.protocol_version == 2 &&
        std::ranges::find(context.capabilities, "dialogue.rechat.scene") != context.capabilities.end()));
    json::Value::Array quests;
    const bool tracking = context.protocol_version == 2 && context.runtime_variant == "flat" &&
        std::ranges::find(context.capabilities, "context.quest_tracking") != context.capabilities.end();
    for(const auto& quest:active_quests) quests.emplace_back(quest_state_value(quest, tracking));
    payload.emplace_back("active_quests",std::move(quests));
    if (!active_quests_observation.empty()) payload.emplace_back("active_quests_observation",std::move(active_quests_observation));
    if (!nearby_items_observation.empty()) payload.emplace_back("nearby_items_observation",std::move(nearby_items_observation));
    if (!points_of_interest_observation.empty()) payload.emplace_back("points_of_interest_observation",std::move(points_of_interest_observation));
    if (!audience_observation.empty()) payload.emplace_back("audience_observation",std::move(audience_observation));
    json::Value::Array items;
    for(const auto& item:nearby_items) items.emplace_back(nearby_item_value(item));
    payload.emplace_back("nearby_items",std::move(items));
    json::Value::Array pois;
    for(const auto& poi:points_of_interest) pois.emplace_back(point_of_interest_value(poi));
    payload.emplace_back("points_of_interest",std::move(pois));
    return encode_event(context,"context",json::Value{std::move(payload)});
}
inline std::string encode_activate(const EventContext& context, const Identity& actor,
                                   std::string source) {
    return encode_event(context,"activate",json::Value{object({
        {"actor",identity_value(actor)}, {"source",std::move(source)}})});
}
// Exact emitted response provenance travels with a queued continuation, never a mutable speaker name.
struct RechatParent final {
    std::string request_id;
    std::string line_id;
    std::optional<std::uint64_t> scene_context_sequence;
};

inline std::string encode_trigger(const EventContext& context, std::string kind,
                                  const Identity& actor,
                                  std::optional<std::uint64_t> game_time_ticks = std::nullopt,
                                  std::optional<std::string> instruction = std::nullopt,
                                  std::optional<std::string> text = std::nullopt,
                                  std::optional<std::string> capture_id = std::nullopt,
                                  std::optional<std::uint64_t> previous_context_sequence = std::nullopt,
                                  std::optional<std::uint64_t> player_event_serial = std::nullopt,
                                  std::optional<RechatParent> rechat_parent = std::nullopt) {
    const bool owned_rechat = context.protocol_version == 2 &&
        std::ranges::find(context.capabilities, "dialogue.rechat.ownership") != context.capabilities.end();
    const bool scene_rechat = std::ranges::find(context.capabilities, "dialogue.rechat.scene") != context.capabilities.end();
    if (scene_rechat && !owned_rechat)
        throw std::invalid_argument{"fresh rechat scene requires parent ownership"};
    if ((rechat_parent && rechat_parent->scene_context_sequence) || (scene_rechat && kind == "rechat")) {
        if (!scene_rechat || !rechat_parent || !rechat_parent->scene_context_sequence ||
            *rechat_parent->scene_context_sequence <= context.context_sequence ||
            *rechat_parent->scene_context_sequence > core::RuntimeGeneration::maximum_wire_value)
            throw std::invalid_argument{"fresh rechat scene must advance the immutable parent context"};
    }
    if (rechat_parent || (owned_rechat && kind == "rechat")) {
        const auto valid_parent_id = [](const std::string& id) {
            const auto alnum = [](unsigned char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            };
            return !id.empty() && id.size() <= 128 && alnum(id.front()) &&
                std::ranges::all_of(id, [&](unsigned char c) { return alnum(c) || c == '.' || c == '_' || c == ':' || c == '-'; });
        };
        if (!owned_rechat || kind != "rechat" || !rechat_parent || context.context_sequence == 0 ||
            std::ranges::find(context.capabilities, "dialogue.turn_ownership") == context.capabilities.end() ||
            !valid_parent_id(rechat_parent->request_id) || !valid_parent_id(rechat_parent->line_id) ||
            rechat_parent->request_id == context.request_id)
            throw std::invalid_argument{"rechat requires an exact native parent response"};
    }
    if (kind == "player_reaction") {
        if (context.protocol_version != 2 || context.runtime_variant != "flat" || instruction || text || capture_id || game_time_ticks ||
            !player_event_serial || *player_event_serial == 0 || *player_event_serial > core::RuntimeGeneration::maximum_wire_value ||
            std::ranges::find(context.capabilities, "context.player_events") == context.capabilities.end() ||
            std::ranges::find(context.capabilities, "dialogue.player_reactions") == context.capabilities.end())
            throw std::invalid_argument{"player reaction requires owned flat event evidence"};
    } else if (player_event_serial) throw std::invalid_argument{"unexpected player event identity"};
    if (kind == "quest_updated") {
        if (context.protocol_version != 2 || context.runtime_variant != "flat" || instruction || text || capture_id ||
            !previous_context_sequence || *previous_context_sequence == 0 || *previous_context_sequence >= context.context_sequence ||
            std::ranges::find(context.capabilities, "dialogue.quest_reactions") == context.capabilities.end() ||
            std::ranges::find(context.capabilities, "context.quest_tracking") == context.capabilities.end())
            throw std::invalid_argument{"quest reaction requires two owned flat observations"};
    } else if (previous_context_sequence) throw std::invalid_argument{"unexpected previous quest observation"};
    if (capture_id && (context.protocol_version != 2 || kind != "external_reaction" || capture_id->empty()))
        throw std::invalid_argument{"capture binding requires a native visual reaction"};
    auto payload = object({{"kind",std::move(kind)}, {"actor",identity_value(actor)}});
    if (game_time_ticks) payload.emplace_back("game_time_ticks", *game_time_ticks);
    if (instruction) payload.emplace_back("instruction", std::move(*instruction));
    if (text) payload.emplace_back("text", std::move(*text));
    if (capture_id) payload.emplace_back("capture_id", std::move(*capture_id));
    if (previous_context_sequence) payload.emplace_back("previous_context_sequence", *previous_context_sequence);
    if (player_event_serial) payload.emplace_back("event_id", "player:" + std::to_string(*player_event_serial));
    if (rechat_parent) {
        payload.emplace_back("reply_to_request_id", std::move(rechat_parent->request_id));
        payload.emplace_back("reply_to_line_id", std::move(rechat_parent->line_id));
        if (rechat_parent->scene_context_sequence)
            payload.emplace_back("scene_context_sequence", *rechat_parent->scene_context_sequence);
    }
    return encode_event(context,"trigger",json::Value{std::move(payload)});
}

inline std::string encode_profile_refresh(const EventContext& context,
                                          const std::vector<Identity>& actors,
                                          bool include_narrator) {
    json::Value::Array actor_values;
    actor_values.reserve(actors.size());
    for (const auto& actor : actors) actor_values.emplace_back(identity_value(actor));
    return encode_event(context, "profile_refresh", json::Value{object({
        {"actors", std::move(actor_values)}, {"include_narrator", include_narrator}
    })});
}
enum class DiaryRole { npc, narrator, player };
enum class DiaryReason { manual, sleep, wait };
struct DiaryRequest final {
    std::vector<Identity> actors;
    bool include_player{};
    bool include_narrator{};
    DiaryReason reason{DiaryReason::manual};
};

// One bounded selection owns the entire admission, including roles disabled by server policy.
inline std::string encode_diary_request(const EventContext& context, const DiaryRequest& request) {
    if (context.protocol_version != 2) throw std::invalid_argument{"native diary requires protocol v2"};
    if (request.actors.size() > 16 || (request.actors.empty() && !request.include_player && !request.include_narrator))
        throw std::invalid_argument{"diary requires a bounded nonempty role selection"};
    const char* reason{};
    switch (request.reason) {
    case DiaryReason::manual: reason = "manual"; break;
    case DiaryReason::sleep: reason = "sleep"; break;
    case DiaryReason::wait: reason = "wait"; break;
    default: throw std::invalid_argument{"unsupported diary reason"};
    }
    const auto same_ascii = [](std::string_view left, std::string_view right) {
        return std::ranges::equal(left, right, [](unsigned char a, unsigned char b) {
            const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
            return fold(a) == fold(b);
        });
    };
    json::Value::Array actors;
    actors.reserve(request.actors.size());
    for (std::size_t i = 0; i < request.actors.size(); ++i) {
        const auto& actor = request.actors[i];
        auto value = identity_value(actor);
        (void)identity(value);
        if (actor.origin_plugin.empty() || actor.origin_plugin.size() > 255 || actor.display_name.size() > 255 ||
            !std::ranges::all_of(std::string_view{actor.form_id}.substr(2), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            })) throw std::invalid_argument{"invalid diary actor identity"};
        if (actor.playthrough_id != request.actors.front().playthrough_id)
            throw std::invalid_argument{"diary actors belong to different playthroughs"};
        for (std::size_t j = 0; j < i; ++j) {
            const auto& previous = request.actors[j];
            if (same_ascii(actor.form_id, previous.form_id) && same_ascii(actor.origin_plugin, previous.origin_plugin))
                throw std::invalid_argument{"duplicate diary actor identity"};
        }
        actors.emplace_back(std::move(value));
    }
    return encode_event(context, "diary_request", json::Value{object({
        {"actors", std::move(actors)}, {"include_player", request.include_player},
        {"include_narrator", request.include_narrator}, {"reason", reason}
    })});
}

inline std::string encode_diary_request(const EventContext& context, const Identity& actor, DiaryRole role = DiaryRole::npc) {
    if (role != DiaryRole::npc && role != DiaryRole::narrator && role != DiaryRole::player)
        throw std::invalid_argument{"unsupported diary role"};
    return encode_diary_request(context, DiaryRequest{
        role == DiaryRole::npc ? std::vector<Identity>{actor} : std::vector<Identity>{},
        role == DiaryRole::player, role == DiaryRole::narrator});
}
inline std::string encode_cancel(const EventContext& context, std::string cancel_request_id) {
    return encode_event(context,"cancel",json::Value{object({{"cancel_request_id",std::move(cancel_request_id)}})});
}
inline std::string encode_halt(const EventContext& context) {
    return encode_event(context,"halt",json::Value{json::Value::Object{}});
}
// Shared validation/serialization keeps byte-bounded local observations identical to the wire.
inline json::Value action_inventory_value(const ActionInventoryObservation& observed, bool extended) {
    if (observed.items.size() > 512 || (observed.items.size() > 32 && !extended) ||
        (observed.observation != "complete" && observed.observation != "partial" && observed.observation != "unavailable") ||
        (observed.observation == "unavailable" && !observed.items.empty()))
        throw std::invalid_argument{"invalid post-action inventory capability, size or quality"};
    const auto valid_identity = [](const Identity& value) {
        const auto alnum = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
        return value.form_id.size() == 10 && value.form_id.starts_with("0x") &&
            std::ranges::all_of(std::string_view{value.form_id}.substr(2), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            }) && value.origin_plugin.size() > 4 && value.origin_plugin.size() <= 255 &&
            value.origin_plugin.find_first_of("/\\") == std::string::npos &&
            (value.origin_plugin.ends_with(".esm") || value.origin_plugin.ends_with(".esp") || value.origin_plugin.ends_with(".esl")) &&
            !value.playthrough_id.empty() && value.playthrough_id.size() <= 128 && alnum(value.playthrough_id.front()) &&
            std::ranges::all_of(value.playthrough_id, [&](char c) { return alnum(c) || c == '.' || c == '_' || c == ':' || c == '-'; }) &&
            value.display_name.size() <= 255;
    };
    if (!valid_identity(observed.actor)) throw std::invalid_argument{"invalid post-action inventory actor"};
    auto actor = identity_value(observed.actor);
    (void)identity(actor);
    json::Value::Array items;
    for (const auto& item : observed.items) {
        if (item.count == 0 || item.count > 2147483647 || item.value < 0 || item.value > 2147483647 ||
            item.form_type > 65535 || !std::isfinite(item.weight) || item.weight < 0 || item.weight > 100000 ||
            item.display_name.find_first_not_of(" \t\r\n\v\f") == std::string::npos || item.display_name.size() > 255 ||
            !valid_identity({item.form_id, item.origin_plugin, observed.actor.playthrough_id, item.display_name}))
            throw std::invalid_argument{"invalid post-action inventory row"};
        (void)identity(identity_value({item.form_id, item.origin_plugin, observed.actor.playthrough_id, item.display_name}));
        items.emplace_back(object({{"form_id",item.form_id},{"origin_plugin",item.origin_plugin},
            {"display_name",item.display_name},{"count",item.count},{"form_type",item.form_type},
            {"value",item.value},{"weight",item.weight},{"equipped",item.equipped}}));
    }
    return json::Value{object({{"actor",std::move(actor)},{"items",std::move(items)},{"observation",observed.observation}})};
}

inline std::string encode_action_result(const ActionResultEvent& e) {
    if (e.inventory && e.transfer) throw std::invalid_argument{"conflicting inventory receipt types"};
    const auto supports = [&](std::string_view capability) { return std::ranges::find(e.capabilities, capability) != e.capabilities.end(); };
    if (e.transfer && (e.runtime_variant != "flat" || !supports("action.transfer_inventory") || !supports("action.give_item_to")))
        throw std::invalid_argument{"unnegotiated transfer receipt"};
    auto result=object({{"schema",e.transfer ? "synth.action.transfer-result.v2" : e.inventory ? "synth.action.result.v2" : "synth.action.result.v1"},{"action_id",e.action_id},{"idempotency_key",e.idempotency_key},{"status",e.status},{"detail",e.detail}});
    if ((e.inventory || e.transfer) && (e.protocol_version != 2 || e.context_sequence == 0 ||
        !supports("action.inventory_observation") || !supports("dialogue.turn_ownership")))
        throw std::invalid_argument{"invalid post-action inventory capability"};
    const bool extended = e.runtime_variant == "flat" && supports("context.inventory_512");
    if (e.inventory) result.emplace_back("inventory", action_inventory_value(*e.inventory, extended));
    if (e.transfer) {
        auto donor = action_inventory_value(e.transfer->donor, extended);
        auto recipient = action_inventory_value(e.transfer->recipient, extended);
        const auto& a = e.transfer->donor.actor;
        const auto& b = e.transfer->recipient.actor;
        // A loaded FormID identifies one reference even if an untrusted plugin label differs.
        if (a.playthrough_id != b.playthrough_id || std::ranges::equal(a.form_id, b.form_id, [](char x, char y) {
                const auto lower = [](char c) { return c >= 'A' && c <= 'F' ? c + ('a' - 'A') : c; };
                return lower(x) == lower(y);
            })) throw std::invalid_argument{"transfer requires distinct actors in one playthrough"};
        result.emplace_back("inventory", std::move(donor));
        result.emplace_back("recipient_inventory", std::move(recipient));
    }
    auto payload=json::Value{object({{"result",json::Value{std::move(result)}}})};
    auto encoded = encode_event(EventContext{e.session_id,e.request_id,e.turn_id,e.runtime_variant,e.client_version,e.runtime_version,e.generation,e.capabilities,e.protocol_version,e.context_sequence},"action_result",std::move(payload));
    // Match ingress's byte budget; never silently truncate observations after snapshot preparation.
    if (encoded.size() > 1024 * 1024) throw std::invalid_argument{"action receipt exceeds ingress byte limit"};
    return encoded;
}

} // namespace synth::protocol_native
