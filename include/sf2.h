#pragma once
#include <stdint.h>

/* ---------------------------------------------------------------
 *  Minimal SF2 (SoundFont 2) parser
 *  Only the fields needed for per-note sample lookup / playback
 * --------------------------------------------------------------- */

/* Generator operation codes used during zone lookup */
#define SF_GEN_START_ADDRS_OFFSET     0
#define SF_GEN_END_ADDRS_OFFSET       1
#define SF_GEN_STARTLOOP_ADDRS_OFFSET 2
#define SF_GEN_ENDLOOP_ADDRS_OFFSET   3
#define SF_GEN_START_ADDRS_COARSE     4
#define SF_GEN_MOD_ENV_TO_PITCH       7
#define SF_GEN_ENDLOOP_ADDRS_COARSE  12
#define SF_GEN_PAN                   17
#define SF_GEN_INIT_ATTENUATION      49
#define SF_GEN_STARTLOOP_ADDRS_COARSE 46
#define SF_GEN_COARSE_TUNE           52
#define SF_GEN_FINE_TUNE             53
#define SF_GEN_SAMPLE_ID             54
#define SF_GEN_SAMPLE_MODES          55
#define SF_GEN_SCALE_TUNING          57
#define SF_GEN_EXCLUSIVE_CLASS       58
#define SF_GEN_OVERRIDING_ROOT_KEY   59
#define SF_GEN_KEY_RANGE             44
#define SF_GEN_VEL_RANGE             45
#define SF_GEN_INSTRUMENT            41

/* sampleModes bit flags */
#define SF_SAMPLE_NO_LOOP     0
#define SF_SAMPLE_LOOP        1
#define SF_SAMPLE_PING_PONG   2

/* sfSampleType flags */
#define SF_STYPE_MONO   1
#define SF_STYPE_RIGHT  2
#define SF_STYPE_LEFT   4
#define SF_STYPE_LINKED 8
#define SF_STYPE_ROM    0x8000

/* ------- Raw structures (packed, matches the SF2 binary layout) ------- */
#pragma pack(push, 1)

typedef struct {
    char     achPresetName[20];
    uint16_t wPreset;       /* MIDI program number 0-127            */
    uint16_t wBank;         /* MIDI bank 0-127                      */
    uint16_t wPresetBagNdx; /* index into PBAG                      */
    uint32_t dwLibrary;
    uint32_t dwGenre;
    uint32_t dwMorphology;
} sfPresetHeader;           /* 38 bytes */

typedef struct {
    uint16_t wGenNdx;
    uint16_t wModNdx;
} sfBag;                    /* 4 bytes */

typedef union {
    struct { uint8_t lo, hi; } range;
    int16_t shAmount;
    uint16_t wAmount;
} sfGenAmount;              /* 2 bytes */

typedef struct {
    uint16_t sfGenOper;
    sfGenAmount genAmount;
} sfGenList;                /* 4 bytes */

typedef struct {
    char     achInstName[20];
    uint16_t wInstBagNdx;
} sfInst;                   /* 22 bytes */

typedef struct {
    char     achSampleName[20];
    uint32_t dwStart;
    uint32_t dwEnd;
    uint32_t dwStartloop;
    uint32_t dwEndloop;
    uint32_t dwSampleRate;
    uint8_t  byOriginalPitch;
    int8_t   chPitchCorrection;
    uint16_t wSampleLink;
    uint16_t sfSampleType;
} sfSample;                 /* 46 bytes */

#pragma pack(pop)

/* ------- High-level zone descriptor returned by the lookup ------- */
typedef struct {
    /* Which sample to play */
    int       sampleIdx;

    /* Pitch parameters */
    int       coarseTune;       /* semitones  */
    int       fineTune;         /* cents      */
    int       scaleTuning;      /* cents/semitone, default 100 */
    int       overridingRootKey;/* -1 = use sample byOriginalPitch */

    /* Loop */
    int       sampleModes;      /* SF_SAMPLE_* */

    /* Volume / pan from instrument generators */
    int       initialAttenuation; /* centi-bels, 0 = full vol */
    int       pan;              /* -500..500 (SF units) */

    /* Addrs offsets (in sample frames, added to sfSample.dwStart etc.) */
    int32_t   startAddrsOffset;
    int32_t   endAddrsOffset;
    int32_t   startloopAddrsOffset;
    int32_t   endloopAddrsOffset;
} SF2Zone;

/* ------- Main SF2 container ------- */
typedef struct {
    /* Preset table */
    sfPresetHeader *presets;
    int             presetCount;

    /* Instrument table (just the headers for bag indexing) */
    sfInst    *instruments;
    int        instrumentCount;

    /* Preset bags + generators */
    sfBag     *presetBags;
    int        presetBagCount;
    sfGenList *presetGens;
    int        presetGenCount;

    /* Instrument bags + generators */
    sfBag     *instBags;
    int        instBagCount;
    sfGenList *instGens;
    int        instGenCount;

    /* Sample headers */
    sfSample  *sampleHeaders;
    int        sampleCount;

    /* Raw PCM sample data (16-bit, mono) */
    int16_t   *sampleData;
    uint32_t   sampleDataLen; /* in samples (int16_t), not bytes */
} SF2;

SF2  *sf2_load(const char *path);
void  sf2_free(SF2 *sf);

/**
 * Find the best matching zone for a given bank/program/note/velocity.
 * Returns 1 on success, 0 if no match found.
 */
int sf2_find_zone(SF2 *sf, int bank, int program,
                  int note, int velocity,
                  SF2Zone *out_zone);
