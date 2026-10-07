# Native event API, version 1

SYNTH.dll and SYNTHVR.dll export `SYNTH_GetExternalAPI`. Native x64 mods can use
`src/integration/synth_external_api.h` without linking CommonLib or sharing game
pointers. This is the native-plugin integration lane. Papyrus bindings and original
source declarations and a reproducible PEX build are now packaged and locally installed
for flat. Actual VM calls and script-runtime acceptance remain unverified
(see `docs/PAPYRUS-API.md`). Recruitment, dismissal, wait and resume remain
unfinished. OpenPrompt has a flat native implementation, pending gameplay acceptance;
VR has no supported prompt presenter yet.

| Kind | Text | Behavior |
| --- | --- | --- |
| `SYNTH_EXTERNAL_SPEAK_EXACT` | Required | Supplied text uses the selected actor's TTS endpoint, without an LLM conversation turn or automatic rechat. |
| `SYNTH_EXTERNAL_COMMENT` | Empty | One contextual in-character response; generated actions and automatic rechat disabled. |
| `SYNTH_EXTERNAL_REACT` | Required | One response to the supplied scene direction; generated actions and automatic rechat disabled. |
| `SYNTH_EXTERNAL_ASK` | Required | Normal player-input conversation for the exact actor, including ordinary action/rechat policy. Text is literal, not a chatbox slash command. |
| `SYNTH_EXTERNAL_OPEN_PROMPT` (5) | Empty | Flat: open an empty actor-bound chatbox. No dialogue is sent until the player submits it. Requires the installed, initialized Menu Framework window. VR: rejected at execution. |

## Calling contract

- Obtain the export from the already-loaded DLL using GetModuleHandle/GetProcAddress.
  Do not load the game plugin yourself. Pass API version 1; other versions return null.
- Initialize the complete request to zero, set `size=sizeof(SynthExternalRequestV1)`,
  select one allowed kind and supply the intended loaded actor's current full FormID.
  Player and zero IDs are rejected. `reserved` must remain zero.
- Read `get_epoch()` immediately before submission. Zero means unavailable. Copy the
  nonzero value into `request.epoch`; do not persist it in saves or reuse it after a gate.
- Text is a NUL-terminated UTF-8 buffer of at most 1000 bytes before trimming. Surrounding
  ASCII space/tab/CR/LF is removed; internal spacing is preserved. Required text must
  remain nonempty. Comment and OpenPrompt must have empty text. Unknown kinds and malformed input fail.
- Calls may originate on other native threads. Keep the input memory valid and immutable
  until submit returns. SYNTH copies it; no input pointer or game object is retained.
- Admission uses a nonblocking 16-slot queue and a two-second lifetime. At most one
  request is considered per safe game pump. Acceptance means queued, not executed,
  delivered, heard or persisted. Busy, expiry, gating and invalidation can discard it.
- One-shot requests require an idle pipeline, including no pending rechat/actions,
  network response or speech. Ask can interrupt a known same-actor turn using the
  ordinary local cancellation, asynchronous server cancellation and playback interruption
  path. Different-actor or narrator/unresolved conversation ownership is rejected even
  between responses. Unknown or mixed outstanding work is not assumed to belong to the
  requested NPC. Active microphone recording still blocks admission for every kind.
- A nonzero epoch now means the request queue is open, not that the pipeline is idle.
  Execution rechecks the request kind and exact canonical actor/playthrough ownership.
  Direct player input may choose a new owner; a matching end-conversation action releases
  ownership. Rechat/speech continuations retain their originating turn's owner.
- Menu, world readiness, session retirement, save/load, halt and relevant combat gates
  invalidate admission. Query a fresh epoch after readiness returns. No automatic retry
  of an accepted request: that can duplicate speech if the original was delivered.
- Execution requires the exact actor in the fresh captured actor set, a loaded 3D node,
  the same interior or exterior worldspace as the player, configured hearing distance,
  usable life/posture, no vanilla player conversation, hostility policy and cooldown.
  VR distance uses a fresh same-frame HMD pose. The adapter directly captures the named
  actor even when nearest-actor discovery omitted it, reserving one of the 64 scene
  slots for that actor. Existing same-frame identity must still match. The selected
  actor is also prioritized in the smaller server context packet. Crosshair/nearest
  substitution is never allowed; unavailable or changed actors are refused. Actual
  crowded-scene game acceptance is still required independently for flat and VR.
- Network/model/TTS work uses the existing owned asynchronous session pipeline. The API
  exposes no arbitrary URL, JSON payload, action name, script or engine command.
- There is no completion callback in version 1. The game-pump log records kind, FormID and
  whether the request entered the session pipeline. Normal request/speech diagnostics
  then apply. A queued request invalidated before the pump has no execution log.

## Actor-bound prompt

OpenPrompt checks the same exact-actor and conversation-owner gates as Ask but does
not start or cancel a turn. A busy draft or unread submission refuses another open.
The render bridge retains copied canonical actor identity and session cancellation,
not an actor pointer or the opening snapshot. Each draft has a unique receipt:
cancel, discard and reopen cannot let an old render callback submit a replacement draft.

Sending captures that exact actor again and rechecks identity, playthrough, generation,
scene freshness, eligibility and current conversation ownership. It never substitutes
the crosshair or nearest NPC. Failure drops the message with a notification. Prompt
text uses the ordinary 4096-byte chatbox limit and is literal, including slash prefixes;
the 1000-byte direct native request bound is unchanged. Cancel sends nothing.

Queued/admitted OpenPrompt is not proof the window was displayed or the player sent
anything. No completion callback is provided. Flat visual/input/save-load acceptance
and an independent VR presenter/acceptance gate remain open.

## Minimal C++ caller

```cpp
#include <Windows.h>
#include <cstring>
#include "synth_external_api.h"

bool RequestComment(uint32_t targetFormId) {
    HMODULE module = GetModuleHandleW(L"SYNTH.dll");
    if (!module) module = GetModuleHandleW(L"SYNTHVR.dll");
    if (!module) return false;
    auto getAPI = reinterpret_cast<SynthGetExternalAPIFn>(
        GetProcAddress(module, "SYNTH_GetExternalAPI"));
    const auto* api = getAPI ? getAPI(SYNTH_EXTERNAL_API_VERSION) : nullptr;
    if (!api || api->size != sizeof(SynthExternalAPIV1)) return false;
    SynthExternalRequestV1 request{};
    request.size = sizeof(request);
    request.kind = SYNTH_EXTERNAL_COMMENT;
    request.actor_form_id = targetFormId;
    request.epoch = api->get_epoch();
    return request.epoch && api->submit(&request) == SYNTH_EXTERNAL_ACCEPTED;
}
```

ABI layout on x64: request 1032 bytes, epoch offset 16, text offset 24; API table 24 bytes.
Return values: accepted 0, invalid 1, unavailable 2, stale 3, busy 4. Never free the API table
or retain it beyond the module lifetime. Neither automatic tests nor a DLL export prove
third-party/gameplay behavior; flat and VR require independent manual acceptance.
