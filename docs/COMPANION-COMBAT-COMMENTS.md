# Post-combat companion comments

`[RpgEvents] CompanionCombatComments=false` is an optional flat-only setting. It
requires the paired server's `dialogue.companion_combat_comments` acknowledgement.
Enabling it replaces generic combat-end reactions; level-up reactions are unchanged.
The server's existing RPG combat-comment enable and chance controls still apply.

The client retains up to eight teammates actually observed fighting during the
player's sampled encounter. After three seconds without observed participant or
player combat, it selects the nearest living registered participant still nearby
and following; equal distances use the reference ID. Busy or ineligible decisions
are consumed. All companions share a 30-second attempt cooldown.

The acknowledged combat-end observation retains typed participant identities and
observation times. The server checks that evidence, the active registration,
current audience and teammate state, event ownership, freshness and first claim.
It records an observation rather than inventing a player utterance. Comments cannot
start actions or rechat. New player input, composing, menus, load, dismissal,
departure or renewed combat cancel only the owned remark. Speech or text must start
within 15 seconds of admission; early scene validation does not count as delivery.

## Limits and acceptance

All automated actors and encounters are synthetic. Flat gameplay and the new
settings row remain unverified. Sampling can miss brief encounters and then emits
no comment. Combat searching is not independently captured. The native teammate
flag and SYNTH wait observation do not establish every vanilla/modded wait or
dismissal behaviour; those require actual game checks. No victory or kill outcome
is inferred. VR does not advertise this capability and sends no companion trigger.

Check duplicate ends, a closer nonparticipant, dismissal, another participant's
renewed combat, input, composing, menus, load, delayed provider/TTS output and normal
Control modes separately in flat gameplay. VR needs its own capture/admission work
and headset acceptance before enabling this feature.
