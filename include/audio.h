#pragma once
#include <stdint.h>
#include "sf2.h"
#include "sfz.h"

/* ---------------------------------------------------------------
 *  OpenAL Soft 3D audio + EFX interface for MI3DI
 * --------------------------------------------------------------- */

#define MI3DI_MAX_VOICES 64   /* simultaneous 3D sources */

/* ---- EFX channel-level effect sends ---- */
typedef struct {
    float reverb_send;  /* 0.0–1.0 (from CC#91 / 127) */
    float chorus_send;  /* 0.0–1.0 (from CC#93 / 127) */
} EffectSends;

/**
 * Initialise OpenAL device, context, EFX effects.
 * Returns 1 on success, 0 on failure.
 */
int  audio_init(void);
void audio_shutdown(void);

/**
 * Compute the 3D position for a note.
 *   x  = pan            (CC#10, 0-127) → -10 .. +10 m
 *   y  = pitch/note num (0-127)        → -10 .. +10 m
 *   z  = volume distance(CC#7,  0-127) →  1  .. 100 m (in front of listener)
 */
void audio_note_position(int pan_cc, int note, int vol_cc,
                         float *x, float *y, float *z);

/**
 * Start playing a single note from SF2 sample data.
 *   Returns a voice handle (≥0) on success, -1 on failure.
 *
 *   note_num  : MIDI note number (for pitch shift)
 *   sf         : loaded SF2
 *   zone       : zone descriptor from sf2_find_zone
 *   x,y,z      : 3D position
 *   sends      : reverb/chorus send levels for this note
 */
int audio_note_on_sf2(int note_num,
                      SF2 *sf, const SF2Zone *zone,
                      float x, float y, float z,
                      const EffectSends *sends);

/**
 * Start playing a single note from an SFZ region (loads WAV on demand).
 *   Returns a voice handle (≥0) on success, -1 on failure.
 */
int audio_note_on_sfz(int note_num,
                      SFZ *sfz, SFZRegion *region,
                      float x, float y, float z,
                      const EffectSends *sends);

/**
 * Release a note (disables looping so the sample plays to its natural end).
 */
void audio_note_off(int voice_handle);

/**
 * Immediately stop and free a voice.
 */
void audio_note_kill(int voice_handle);

/**
 * Stop all currently playing voices.
 */
void audio_all_notes_off(void);

/**
 * Update reverb decay / send mix (maps CC#91 globally; EFX slot level).
 */
void audio_set_reverb_level(float level); /* 0.0–1.0 */

/**
 * Update chorus depth / send mix (maps CC#93 globally; EFX slot level).
 */
void audio_set_chorus_level(float level); /* 0.0–1.0 */

/**
 * Update the effect sends on an already-playing voice (e.g. after CC change).
 */
void audio_update_sends(int voice_handle, const EffectSends *sends);

/**
 * Housekeeping: free any voices whose AL sources have finished naturally.
 * Call periodically (e.g. from MIDI processing thread every ~50 ms).
 */
void audio_collect_finished(void);
