/*
 * MI3DI – OpenAL Soft 3D audio engine
 *
 * Each active MIDI note gets its own AL source placed at a unique
 * 3D position derived from:
 *   X  = MIDI pan    (CC#10)  → -10 .. +10 m
 *   Y  = MIDI note   (0-127) → -10 .. +10 m
 *   Z  = MIDI volume (CC#7)  →   1 .. 100 m in front of listener
 *        (volume maps to distance; actual gain comes from 3D attenuation)
 *
 * EFX effects:
 *   AL_EFFECT_EAXREVERB – mapped from CC#91 (Effect 1 Depth / Reverb)
 *   AL_EFFECT_CHORUS    – mapped from CC#93 (Effect 3 Depth / Chorus)
 */
#include "audio.h"
#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>
#include <AL/efx-presets.h>

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  EFX function pointers                                              */
/* ------------------------------------------------------------------ */
static LPALGENEFFECTS           palGenEffects;
static LPALDELETEEFFECTS        palDeleteEffects;
static LPALEFFECTI              palEffecti;
static LPALEFFECTF              palEffectf;
static LPALEFFECTFV             palEffectfv;
static LPALGENAUXILIARYEFFECTSLOTS  palGenAuxiliaryEffectSlots;
static LPALDELETEAUXILIARYEFFECTSLOTS palDeleteAuxiliaryEffectSlots;
static LPALAUXILIARYEFFECTSLOTI palAuxiliaryEffectSloti;
static LPALAUXILIARYEFFECTSLOTF palAuxiliaryEffectSlotf;
static LPALGENFILTERS           palGenFilters;
static LPALDELETEFILTERS        palDeleteFilters;
static LPALFILTERI              palFilteri;
static LPALFILTERF              palFilterf;
static LPALSOURCE3I             palSource3i;   /* for sends */

static int g_efx_ok = 0;

#define LOAD_EFX(T, name) \
    do { \
        pal##name = (T)alGetProcAddress("al" #name); \
        if (!pal##name) { LOG("EFX: missing al" #name); g_efx_ok = 0; } \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Global OpenAL state                                                */
/* ------------------------------------------------------------------ */
static ALCdevice  *g_device  = NULL;
static ALCcontext *g_context = NULL;

/* EFX effect objects */
static ALuint g_reverb_effect = 0;
static ALuint g_reverb_slot   = 0;
static ALuint g_chorus_effect = 0;
static ALuint g_chorus_slot   = 0;

/* Null filter (pass-through) */
static ALuint g_null_filter = 0;

/* ------------------------------------------------------------------ */
/*  Voice pool                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    int    active;
    ALuint source;
    ALuint buffer;          /* main PCM buffer (may be 0 for streams) */
    int    looping;         /* whether loop was enabled on note-on    */
} Voice;

static Voice g_voices[MI3DI_MAX_VOICES];
static CRITICAL_SECTION g_voices_cs;

/* ------------------------------------------------------------------ */
/*  WAV loader for SFZ samples                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
} WavFmt;

static void *load_wav(const char *path, WavFmt *fmt, uint32_t *pcm_bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("load_wav: cannot open '%s'", path); return NULL; }

    char riff[4]; uint32_t sz; char wave[4];
    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4)) goto bad;
    fread(&sz, 4, 1, f);
    if (fread(wave, 1, 4, f) != 4 || memcmp(wave, "WAVE", 4)) goto bad;

    /* Scan for fmt  and data chunks */
    int got_fmt = 0;
    void *pcm = NULL;
    *pcm_bytes = 0;

    while (!feof(f)) {
        char id[4]; uint32_t csz;
        if (fread(id,  1, 4, f) != 4) break;
        if (fread(&csz, 4, 1, f) != 1) break;

        if (!memcmp(id, "fmt ", 4)) {
            if (csz < 16) { fseek(f, csz, SEEK_CUR); continue; }
            fread(fmt, sizeof(WavFmt), 1, f);
            if (csz > sizeof(WavFmt)) fseek(f, csz - sizeof(WavFmt), SEEK_CUR);
            got_fmt = 1;
        } else if (!memcmp(id, "data", 4)) {
            pcm = malloc(csz);
            if (!pcm) break;
            *pcm_bytes = csz;
            if (fread(pcm, 1, csz, f) != csz) { free(pcm); pcm = NULL; }
            break;
        } else {
            fseek(f, csz, SEEK_CUR);
        }
    }

    fclose(f);
    if (!got_fmt || !pcm) { free(pcm); return NULL; }
    return pcm;

bad:
    fclose(f);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  EFX initialisation                                                 */
/* ------------------------------------------------------------------ */
static void init_efx(void)
{
    if (!alcIsExtensionPresent(g_device, "ALC_EXT_EFX")) {
        LOG("EFX: ALC_EXT_EFX not available – effects disabled");
        return;
    }

    g_efx_ok = 1;
    LOAD_EFX(LPALGENEFFECTS,               GenEffects);
    LOAD_EFX(LPALDELETEEFFECTS,            DeleteEffects);
    LOAD_EFX(LPALEFFECTI,                  Effecti);
    LOAD_EFX(LPALEFFECTF,                  Effectf);
    LOAD_EFX(LPALEFFECTFV,                 Effectfv);
    LOAD_EFX(LPALGENAUXILIARYEFFECTSLOTS,  GenAuxiliaryEffectSlots);
    LOAD_EFX(LPALDELETEAUXILIARYEFFECTSLOTS, DeleteAuxiliaryEffectSlots);
    LOAD_EFX(LPALAUXILIARYEFFECTSLOTI,     AuxiliaryEffectSloti);
    LOAD_EFX(LPALAUXILIARYEFFECTSLOTF,     AuxiliaryEffectSlotf);
    LOAD_EFX(LPALGENFILTERS,              GenFilters);
    LOAD_EFX(LPALDELETEFILTERS,           DeleteFilters);
    LOAD_EFX(LPALFILTERI,                 Filteri);
    LOAD_EFX(LPALFILTERF,                 Filterf);
    LOAD_EFX(LPALSOURCE3I,               Source3i);

    if (!g_efx_ok) return;

    /* ---- Reverb (EAX Reverb for better quality) ---- */
    palGenEffects(1, &g_reverb_effect);
    palEffecti(g_reverb_effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);

    EFXEAXREVERBPROPERTIES reverb = EFX_REVERB_PRESET_ROOM;
    palEffectf (g_reverb_effect, AL_EAXREVERB_DENSITY,         reverb.flDensity);
    palEffectf (g_reverb_effect, AL_EAXREVERB_DIFFUSION,       reverb.flDiffusion);
    palEffectf (g_reverb_effect, AL_EAXREVERB_GAIN,            reverb.flGain);
    palEffectf (g_reverb_effect, AL_EAXREVERB_GAINHF,          reverb.flGainHF);
    palEffectf (g_reverb_effect, AL_EAXREVERB_GAINLF,          reverb.flGainLF);
    palEffectf (g_reverb_effect, AL_EAXREVERB_DECAY_TIME,      reverb.flDecayTime);
    palEffectf (g_reverb_effect, AL_EAXREVERB_DECAY_HFRATIO,   reverb.flDecayHFRatio);
    palEffectf (g_reverb_effect, AL_EAXREVERB_DECAY_LFRATIO,   reverb.flDecayLFRatio);
    palEffectf (g_reverb_effect, AL_EAXREVERB_REFLECTIONS_GAIN,reverb.flReflectionsGain);
    palEffectf (g_reverb_effect, AL_EAXREVERB_REFLECTIONS_DELAY,reverb.flReflectionsDelay);
    palEffectfv(g_reverb_effect, AL_EAXREVERB_REFLECTIONS_PAN, reverb.flReflectionsPan);
    palEffectf (g_reverb_effect, AL_EAXREVERB_LATE_REVERB_GAIN,reverb.flLateReverbGain);
    palEffectf (g_reverb_effect, AL_EAXREVERB_LATE_REVERB_DELAY,reverb.flLateReverbDelay);
    palEffectfv(g_reverb_effect, AL_EAXREVERB_LATE_REVERB_PAN, reverb.flLateReverbPan);
    palEffectf (g_reverb_effect, AL_EAXREVERB_ECHO_TIME,       reverb.flEchoTime);
    palEffectf (g_reverb_effect, AL_EAXREVERB_ECHO_DEPTH,      reverb.flEchoDepth);
    palEffectf (g_reverb_effect, AL_EAXREVERB_MODULATION_TIME, reverb.flModulationTime);
    palEffectf (g_reverb_effect, AL_EAXREVERB_MODULATION_DEPTH,reverb.flModulationDepth);
    palEffectf (g_reverb_effect, AL_EAXREVERB_AIR_ABSORPTION_GAINHF,
                                                               reverb.flAirAbsorptionGainHF);
    palEffectf (g_reverb_effect, AL_EAXREVERB_HFREFERENCE,    reverb.flHFReference);
    palEffectf (g_reverb_effect, AL_EAXREVERB_LFREFERENCE,    reverb.flLFReference);
    palEffectf (g_reverb_effect, AL_EAXREVERB_ROOM_ROLLOFF_FACTOR,
                                                               reverb.flRoomRolloffFactor);

    palGenAuxiliaryEffectSlots(1, &g_reverb_slot);
    palAuxiliaryEffectSloti(g_reverb_slot, AL_EFFECTSLOT_EFFECT, (ALint)g_reverb_effect);
    palAuxiliaryEffectSlotf(g_reverb_slot, AL_EFFECTSLOT_GAIN, 0.0f); /* start muted */

    /* ---- Chorus ---- */
    palGenEffects(1, &g_chorus_effect);
    palEffecti(g_chorus_effect, AL_EFFECT_TYPE, AL_EFFECT_CHORUS);
    palEffecti(g_chorus_effect, AL_CHORUS_WAVEFORM, AL_CHORUS_WAVEFORM_SINUSOID);
    palEffecti(g_chorus_effect, AL_CHORUS_PHASE,    90);
    palEffectf (g_chorus_effect, AL_CHORUS_RATE,     1.1f);
    palEffectf (g_chorus_effect, AL_CHORUS_DEPTH,    0.1f);
    palEffectf (g_chorus_effect, AL_CHORUS_FEEDBACK, 0.25f);
    palEffectf (g_chorus_effect, AL_CHORUS_DELAY,    0.016f);

    palGenAuxiliaryEffectSlots(1, &g_chorus_slot);
    palAuxiliaryEffectSloti(g_chorus_slot, AL_EFFECTSLOT_EFFECT, (ALint)g_chorus_effect);
    palAuxiliaryEffectSlotf(g_chorus_slot, AL_EFFECTSLOT_GAIN, 0.0f);

    /* Null (pass-through) filter for sends where filtering isn't wanted */
    palGenFilters(1, &g_null_filter);
    palFilteri(g_null_filter, AL_FILTER_TYPE, AL_FILTER_NULL);

    LOG("EFX: reverb and chorus effects ready");
}

/* ------------------------------------------------------------------ */
/*  audio_init / audio_shutdown                                        */
/* ------------------------------------------------------------------ */
int audio_init(void)
{
    g_device = alcOpenDevice(NULL);
    if (!g_device) {
        LOG("audio_init: alcOpenDevice failed");
        return 0;
    }

    /* Request EFX with enough auxiliary sends (4) */
    ALCint attrs[] = { ALC_MAX_AUXILIARY_SENDS, 4, 0 };
    g_context = alcCreateContext(g_device, attrs);
    if (!g_context) {
        LOG("audio_init: alcCreateContext failed");
        alcCloseDevice(g_device);
        g_device = NULL;
        return 0;
    }
    alcMakeContextCurrent(g_context);

    /* Listener at origin, facing +Z */
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    ALfloat orient[] = { 0.0f, 0.0f, 1.0f,   /* at (forward) */
                         0.0f, 1.0f, 0.0f };  /* up           */
    alListenerfv(AL_ORIENTATION, orient);
    alListenerf(AL_GAIN, 1.0f);

    /* Inverse distance clamped – natural roll-off */
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);

    InitializeCriticalSection(&g_voices_cs);
    memset(g_voices, 0, sizeof(g_voices));

    init_efx();

    LOG("audio_init: OpenAL ready  device='%s'",
        alcGetString(g_device, ALC_DEVICE_SPECIFIER));
    return 1;
}

void audio_shutdown(void)
{
    audio_all_notes_off();

    if (g_efx_ok) {
        if (g_null_filter)   { palDeleteFilters(1, &g_null_filter);   g_null_filter   = 0; }
        if (g_reverb_slot)   { palDeleteAuxiliaryEffectSlots(1, &g_reverb_slot);  g_reverb_slot   = 0; }
        if (g_chorus_slot)   { palDeleteAuxiliaryEffectSlots(1, &g_chorus_slot);  g_chorus_slot   = 0; }
        if (g_reverb_effect) { palDeleteEffects(1, &g_reverb_effect); g_reverb_effect = 0; }
        if (g_chorus_effect) { palDeleteEffects(1, &g_chorus_effect); g_chorus_effect = 0; }
    }

    DeleteCriticalSection(&g_voices_cs);

    if (g_context) { alcMakeContextCurrent(NULL); alcDestroyContext(g_context); g_context = NULL; }
    if (g_device)  { alcCloseDevice(g_device); g_device = NULL; }
    LOG("audio_shutdown: complete");
}

/* ------------------------------------------------------------------ */
/*  3D position helper                                                 */
/* ------------------------------------------------------------------ */
void audio_note_position(int pan_cc, int note, int vol_cc,
                         float *x, float *y, float *z)
{
    /*  pan_cc : 0-127  →  -10 .. +10 m  (64=centre) */
    *x = ((float)pan_cc - 64.0f) / 64.0f * 10.0f;

    /*  note   : 0-127  →  -10 .. +10 m  (64=centre)  */
    *y = ((float)note - 64.0f) / 64.0f * 10.0f;

    /*  vol_cc : 0-127  →  distance 100m .. 1m         */
    /*  (high volume = close to listener)               */
    float t = (float)vol_cc / 127.0f;             /* 0=silent, 1=max vol */
    *z = 1.0f + (1.0f - t) * 99.0f;              /* 1m .. 100m          */
}

/* ------------------------------------------------------------------ */
/*  Voice helpers                                                      */
/* ------------------------------------------------------------------ */
static int alloc_voice(void)
{
    for (int i = 0; i < MI3DI_MAX_VOICES; i++) {
        if (!g_voices[i].active) return i;
    }
    return -1;
}

static void connect_sends(ALuint source, const EffectSends *sends)
{
    if (!g_efx_ok) return;

    /* Send 0: reverb */
    palSource3i(source, AL_AUXILIARY_SEND_FILTER,
                (ALint)g_reverb_slot, 0, (ALint)g_null_filter);
    alSourcef(source, AL_ROOM_ROLLOFF_FACTOR, sends->reverb_send);

    /* Send 1: chorus */
    palSource3i(source, AL_AUXILIARY_SEND_FILTER,
                (ALint)g_chorus_slot, 1, (ALint)g_null_filter);
    /* Chorus is controlled via slot gain – per-source chorus depth not directly
     * exposed in basic EFX, so we modulate the slot gain per-update */
    (void)sends->chorus_send; /* handled by audio_set_chorus_level globally */
}

static void setup_source(ALuint source, float x, float y, float z,
                         const EffectSends *sends)
{
    alSource3f(source, AL_POSITION, x, y, z);
    alSource3f(source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    alSourcef (source, AL_REFERENCE_DISTANCE, 1.0f);
    alSourcef (source, AL_MAX_DISTANCE,      200.0f);
    alSourcef (source, AL_ROLLOFF_FACTOR,    1.0f);

    /* Ignore MIDI velocity for gain – let 3D distance do it */
    alSourcef(source, AL_GAIN, 1.0f);
    alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE);

    connect_sends(source, sends);
}

/* ------------------------------------------------------------------ */
/*  audio_note_on_sf2                                                  */
/* ------------------------------------------------------------------ */
int audio_note_on_sf2(int note_num,
                      SF2 *sf, const SF2Zone *zone,
                      float x, float y, float z,
                      const EffectSends *sends)
{
    if (!sf || !zone || zone->sampleIdx < 0 ||
        zone->sampleIdx >= sf->sampleCount)
        return -1;

    sfSample *sh = &sf->sampleHeaders[zone->sampleIdx];

    /* Skip ROM / linked samples */
    if (sh->sfSampleType & SF_STYPE_ROM) return -1;

    /* Resolve sample data bounds */
    uint32_t s_start = sh->dwStart + (uint32_t)zone->startAddrsOffset;
    uint32_t s_end   = sh->dwEnd   + (uint32_t)zone->endAddrsOffset;
    if (s_end > sf->sampleDataLen) s_end = sf->sampleDataLen;
    if (s_start >= s_end) return -1;

    uint32_t frame_count = s_end - s_start;

    /* Build loop points */
    int loop = (zone->sampleModes & SF_SAMPLE_LOOP) != 0;
    uint32_t loop_start = sh->dwStartloop + (uint32_t)zone->startloopAddrsOffset;
    uint32_t loop_end   = sh->dwEndloop   + (uint32_t)zone->endloopAddrsOffset;
    if (loop_start < s_start || loop_end > s_end ||
        loop_start >= loop_end) loop = 0;

    /* Compute pitch adjustment */
    int root_key = (zone->overridingRootKey >= 0) ?
                    zone->overridingRootKey :
                    (int)sh->byOriginalPitch;

    float semitones = (float)(note_num - root_key)
                    * ((float)zone->scaleTuning / 100.0f)
                    + (float)zone->coarseTune
                    + (float)zone->fineTune / 100.0f
                    + (float)sh->chPitchCorrection / 100.0f;

    float pitch = powf(2.0f, semitones / 12.0f);

    /* Allocate voice */
    EnterCriticalSection(&g_voices_cs);
    int vi = alloc_voice();
    if (vi < 0) {
        LeaveCriticalSection(&g_voices_cs);
        LOG("audio_note_on_sf2: voice pool exhausted");
        return -1;
    }

    /* Create AL buffer */
    ALuint buf;
    alGenBuffers(1, &buf);
    alBufferData(buf,
                 AL_FORMAT_MONO16,
                 sf->sampleData + s_start,
                 (ALsizei)(frame_count * sizeof(int16_t)),
                 (ALsizei)sh->dwSampleRate);

    /* Create AL source */
    ALuint src;
    alGenSources(1, &src);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcei(src, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    alSourcef(src, AL_PITCH, pitch);

    setup_source(src, x, y, z, sends);
    alSourcePlay(src);

    g_voices[vi].active  = 1;
    g_voices[vi].source  = src;
    g_voices[vi].buffer  = buf;
    g_voices[vi].looping = loop;

    LeaveCriticalSection(&g_voices_cs);

    LOG("audio_note_on_sf2: vi=%d note=%d pitch=%.3f "
        "pos=(%.1f,%.1f,%.1f) loop=%d",
        vi, note_num, pitch, x, y, z, loop);
    return vi;
}

/* ------------------------------------------------------------------ */
/*  audio_note_on_sfz                                                  */
/* ------------------------------------------------------------------ */
int audio_note_on_sfz(int note_num,
                      SFZ *sfz, SFZRegion *region,
                      float x, float y, float z,
                      const EffectSends *sends)
{
    if (!sfz || !region || !region->sample[0]) return -1;

    /* Build absolute path */
    char fullpath[SFZ_MAX_PATH * 2];
    snprintf(fullpath, sizeof(fullpath), "%s%s",
             sfz->basedir, region->sample);

    /* Load WAV */
    WavFmt fmt;
    uint32_t pcm_bytes;
    void *pcm = load_wav(fullpath, &fmt, &pcm_bytes);
    if (!pcm) {
        LOG("audio_note_on_sfz: failed to load '%s'", fullpath);
        return -1;
    }

    /* Determine AL format */
    ALenum al_fmt;
    if      (fmt.numChannels == 1 && fmt.bitsPerSample == 8)  al_fmt = AL_FORMAT_MONO8;
    else if (fmt.numChannels == 1 && fmt.bitsPerSample == 16) al_fmt = AL_FORMAT_MONO16;
    else if (fmt.numChannels == 2 && fmt.bitsPerSample == 8)  al_fmt = AL_FORMAT_STEREO8;
    else if (fmt.numChannels == 2 && fmt.bitsPerSample == 16) al_fmt = AL_FORMAT_STEREO16;
    else {
        LOG("audio_note_on_sfz: unsupported format ch=%d bits=%d",
            fmt.numChannels, fmt.bitsPerSample);
        free(pcm);
        return -1;
    }

    /* Pitch: semitones between note and root key */
    float semitones = (float)(note_num - region->pitch_keycenter)
                    + (float)region->transpose
                    + region->tune / 100.0f;
    float pitch = powf(2.0f, semitones / 12.0f);

    int loop = (region->loop_mode == 1);

    EnterCriticalSection(&g_voices_cs);
    int vi = alloc_voice();
    if (vi < 0) {
        LeaveCriticalSection(&g_voices_cs);
        free(pcm);
        return -1;
    }

    ALuint buf;
    alGenBuffers(1, &buf);
    alBufferData(buf, al_fmt, pcm, (ALsizei)pcm_bytes, (ALsizei)fmt.sampleRate);
    free(pcm);

    ALuint src;
    alGenSources(1, &src);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcei(src, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    alSourcef(src, AL_PITCH, pitch);

    setup_source(src, x, y, z, sends);
    alSourcePlay(src);

    g_voices[vi].active  = 1;
    g_voices[vi].source  = src;
    g_voices[vi].buffer  = buf;
    g_voices[vi].looping = loop;

    LeaveCriticalSection(&g_voices_cs);

    LOG("audio_note_on_sfz: vi=%d note=%d pitch=%.3f pos=(%.1f,%.1f,%.1f)",
        vi, note_num, pitch, x, y, z);
    return vi;
}

/* ------------------------------------------------------------------ */
/*  Note off / kill                                                    */
/* ------------------------------------------------------------------ */
void audio_note_off(int vi)
{
    if (vi < 0 || vi >= MI3DI_MAX_VOICES) return;
    EnterCriticalSection(&g_voices_cs);
    if (g_voices[vi].active && g_voices[vi].looping) {
        alSourcei(g_voices[vi].source, AL_LOOPING, AL_FALSE);
        g_voices[vi].looping = 0;
        LOG("audio_note_off: vi=%d – released loop", vi);
    }
    LeaveCriticalSection(&g_voices_cs);
}

void audio_note_kill(int vi)
{
    if (vi < 0 || vi >= MI3DI_MAX_VOICES) return;
    EnterCriticalSection(&g_voices_cs);
    if (g_voices[vi].active) {
        alSourceStop(g_voices[vi].source);
        alDeleteSources(1, &g_voices[vi].source);
        alDeleteBuffers(1, &g_voices[vi].buffer);
        memset(&g_voices[vi], 0, sizeof(g_voices[vi]));
        LOG("audio_note_kill: vi=%d", vi);
    }
    LeaveCriticalSection(&g_voices_cs);
}

void audio_all_notes_off(void)
{
    EnterCriticalSection(&g_voices_cs);
    for (int i = 0; i < MI3DI_MAX_VOICES; i++) {
        if (g_voices[i].active) {
            alSourceStop(g_voices[i].source);
            alDeleteSources(1, &g_voices[i].source);
            alDeleteBuffers(1, &g_voices[i].buffer);
            memset(&g_voices[i], 0, sizeof(g_voices[i]));
        }
    }
    LeaveCriticalSection(&g_voices_cs);
    LOG("audio_all_notes_off");
}

/* ------------------------------------------------------------------ */
/*  Effect level updates                                               */
/* ------------------------------------------------------------------ */
void audio_set_reverb_level(float level)
{
    if (!g_efx_ok || !g_reverb_slot) return;
    float clamped = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
    palAuxiliaryEffectSlotf(g_reverb_slot, AL_EFFECTSLOT_GAIN, clamped);
    LOG("audio_set_reverb_level: %.3f", clamped);
}

void audio_set_chorus_level(float level)
{
    if (!g_efx_ok || !g_chorus_slot) return;
    float clamped = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
    palAuxiliaryEffectSlotf(g_chorus_slot, AL_EFFECTSLOT_GAIN, clamped);
    LOG("audio_set_chorus_level: %.3f", clamped);
}

void audio_update_sends(int vi, const EffectSends *sends)
{
    if (vi < 0 || vi >= MI3DI_MAX_VOICES) return;
    EnterCriticalSection(&g_voices_cs);
    if (g_voices[vi].active)
        connect_sends(g_voices[vi].source, sends);
    LeaveCriticalSection(&g_voices_cs);
}

/* ------------------------------------------------------------------ */
/*  Housekeeping                                                       */
/* ------------------------------------------------------------------ */
void audio_collect_finished(void)
{
    EnterCriticalSection(&g_voices_cs);
    for (int i = 0; i < MI3DI_MAX_VOICES; i++) {
        if (!g_voices[i].active) continue;
        ALint state;
        alGetSourcei(g_voices[i].source, AL_SOURCE_STATE, &state);
        if (state == AL_STOPPED) {
            alDeleteSources(1, &g_voices[i].source);
            alDeleteBuffers(1, &g_voices[i].buffer);
            memset(&g_voices[i], 0, sizeof(g_voices[i]));
        }
    }
    LeaveCriticalSection(&g_voices_cs);
}
