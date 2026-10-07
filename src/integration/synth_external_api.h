#ifndef SYNTH_EXTERNAL_API_H
#define SYNTH_EXTERNAL_API_H
#include <stdint.h>

/* Native x64 ABI. Obtain SYNTH_GetExternalAPI from the already-loaded SYNTH.dll
 * or SYNTHVR.dll. No game pointers, callbacks, URLs or arbitrary actions cross it. */
#define SYNTH_EXTERNAL_API_VERSION 1u
#define SYNTH_EXTERNAL_SPEAK_EXACT 1u
#define SYNTH_EXTERNAL_COMMENT 2u
#define SYNTH_EXTERNAL_REACT 3u
#define SYNTH_EXTERNAL_ASK 4u
#define SYNTH_EXTERNAL_OPEN_PROMPT 5u
#define SYNTH_EXTERNAL_ACCEPTED 0u
#define SYNTH_EXTERNAL_INVALID 1u
#define SYNTH_EXTERNAL_UNAVAILABLE 2u
#define SYNTH_EXTERNAL_STALE 3u
#define SYNTH_EXTERNAL_BUSY 4u

typedef struct SynthExternalRequestV1 {
    uint32_t size;
    uint32_t kind;
    uint32_t actor_form_id;
    uint32_t reserved;
    uint64_t epoch;
    char text[1001];
} SynthExternalRequestV1;

typedef struct SynthExternalAPIV1 {
    uint32_t size;
    uint32_t version;
    uint64_t (*get_epoch)(void); /* Zero means admission is unavailable. */
    uint32_t (*submit)(const SynthExternalRequestV1* request);
} SynthExternalAPIV1;

typedef const SynthExternalAPIV1* (*SynthGetExternalAPIFn)(uint32_t version);
#endif
