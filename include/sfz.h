#pragma once
#include <stdint.h>

/* ---------------------------------------------------------------
 *  Minimal SFZ parser
 *  Supports the most common opcodes needed for basic playback
 * --------------------------------------------------------------- */

#define SFZ_MAX_PATH 512

typedef struct {
    char   sample[SFZ_MAX_PATH]; /* path to sample file, relative to SFZ */
    int    lokey;                /* 0-127, inclusive range */
    int    hikey;
    int    lovel;
    int    hivel;
    int    pitch_keycenter;      /* root key of the sample */
    float  tune;                 /* fine tune in cents     */
    int    transpose;            /* coarse tune in semitones */
    int    loop_mode;            /* 0=no_loop,1=loop,2=one_shot */
    int    loop_start;
    int    loop_end;
    float  volume;               /* dB offset */
    float  pan;                  /* -100..100 */
} SFZRegion;

typedef struct {
    SFZRegion *regions;
    int        count;
    char       basedir[SFZ_MAX_PATH]; /* directory containing .sfz */
} SFZ;

SFZ  *sfz_load(const char *path);
void  sfz_free(SFZ *sfz);

/**
 * Returns the region matching the given note and velocity, or NULL.
 */
SFZRegion *sfz_find_region(SFZ *sfz, int note, int velocity);
