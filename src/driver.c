/*
 * MI3DI – Windows WinMM MIDI output minidriver
 *
 * Implements the DriverProc / modMessage interface expected by winmm.dll
 * when this DLL is registered under:
 *   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32
 *   "midi<N>" = "mi3di.dll"
 *
 * Per-note 3D spatialization:
 *   X  = CC#10 pan            (0-127 → -10 .. +10 m)
 *   Y  = MIDI note number     (0-127 → -10 .. +10 m)
 *   Z  = CC#7  volume/distance(0-127 →  1  .. 100 m, high=close)
 *
 * EFX send routing:
 *   CC#91  (Reverb Depth)  → reverb aux slot gain
 *   CC#93  (Chorus Depth)  → chorus aux slot gain
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <mmddk.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "version.h"
#include "log.h"
#include "sf2.h"
#include "sfz.h"
#include "audio.h"

/* ------------------------------------------------------------------ */
/*  Soundfont search                                                   */
/* ------------------------------------------------------------------ */
#define MAX_VOICES_PER_CHANNEL 16

typedef enum { SF_NONE, SF_SF2, SF_SFZ } SFType;

static SFType g_sftype   = SF_NONE;
static SF2   *g_sf2      = NULL;
static SFZ   *g_sfz      = NULL;

/* ------------------------------------------------------------------ */
/*  Per-channel MIDI state                                             */
/* ------------------------------------------------------------------ */
#define MIDI_CHANNELS 16

typedef struct {
    int program;       /* 0-127 */
    int bank_msb;      /* CC#0  */
    int bank_lsb;      /* CC#32 */
    int pan;           /* CC#10, 0-127, default 64 */
    int volume;        /* CC#7,  0-127, default 100 */
    int expression;    /* CC#11, 0-127, default 127 */
    int reverb;        /* CC#91, 0-127, default 0   */
    int chorus;        /* CC#93, 0-127, default 0   */
} ChannelState;

static ChannelState g_ch[MIDI_CHANNELS];

/* ------------------------------------------------------------------ */
/*  Active note tracking                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    int used;
    int channel;
    int note;
    int voice_handle; /* returned by audio_note_on_* */
} NoteSlot;

#define MAX_NOTE_SLOTS (MIDI_CHANNELS * MAX_VOICES_PER_CHANNEL)
static NoteSlot g_note_slots[MAX_NOTE_SLOTS];
static CRITICAL_SECTION g_note_cs;

/* Housekeeping timer */
static HANDLE g_timer_thread = NULL;
static volatile int g_timer_run = 0;

/* ------------------------------------------------------------------ */
/*  Soundfont loading                                                  */
/* ------------------------------------------------------------------ */
static int try_sf2(const char *path)
{
    LOG("try_sf2: '%s'", path);
    SF2 *sf = sf2_load(path);
    if (!sf) return 0;
    g_sf2   = sf;
    g_sftype= SF_SF2;
    return 1;
}

static int try_sfz(const char *path)
{
    LOG("try_sfz: '%s'", path);
    SFZ *sfz = sfz_load(path);
    if (!sfz) return 0;
    g_sfz   = sfz;
    g_sftype= SF_SFZ;
    return 1;
}

static void load_soundfont(void)
{
    char path[MAX_PATH];

    /* 1. MI3DI_SOUNDFONT environment variable */
    if (GetEnvironmentVariableA("MI3DI_SOUNDFONT", path, sizeof(path))) {
        if (strstr(path, ".sfz") || strstr(path, ".SFZ")) {
            if (try_sfz(path)) return;
        } else {
            if (try_sf2(path)) return;
        }
    }

    /* 2. %APPDATA%\MI3DI\default.sf2 / .sfz */
    {
        char appdata[MAX_PATH];
        if (GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata))) {
            snprintf(path, sizeof(path), "%s\\MI3DI\\default.sf2", appdata);
            if (try_sf2(path)) return;
            snprintf(path, sizeof(path), "%s\\MI3DI\\default.sfz", appdata);
            if (try_sfz(path)) return;
        }
    }

    /* 3. Same directory as mi3di.dll */
    {
        char dllpath[MAX_PATH];
        HMODULE hmod = GetModuleHandleA("mi3di.dll");
        if (hmod && GetModuleFileNameA(hmod, dllpath, sizeof(dllpath))) {
            char *sep = strrchr(dllpath, '\\');
            if (sep) {
                *(sep + 1) = '\0';
                snprintf(path, sizeof(path), "%sdefault.sf2", dllpath);
                if (try_sf2(path)) return;
                snprintf(path, sizeof(path), "%sdefault.sfz", dllpath);
                if (try_sfz(path)) return;
                /* Also try GeneralUser GS or FluidR3 by name */
                snprintf(path, sizeof(path), "%sGeneralUser GS v1.471.sf2", dllpath);
                if (try_sf2(path)) return;
                snprintf(path, sizeof(path), "%sFluidR3_GM.sf2", dllpath);
                if (try_sf2(path)) return;
            }
        }
    }

    /* 4. %WINDIR%\System32 */
    {
        char sysdir[MAX_PATH];
        GetSystemDirectoryA(sysdir, sizeof(sysdir));
        snprintf(path, sizeof(path), "%s\\mi3di.sf2", sysdir);
        if (try_sf2(path)) return;
        snprintf(path, sizeof(path), "%s\\mi3di.sfz", sysdir);
        if (try_sfz(path)) return;
    }

    LOG("load_soundfont: no soundfont found – set MI3DI_SOUNDFONT or place "
        "default.sf2 / default.sfz beside mi3di.dll");
}

/* ------------------------------------------------------------------ */
/*  Housekeeping timer thread                                          */
/* ------------------------------------------------------------------ */
static DWORD WINAPI timer_thread_proc(LPVOID unused)
{
    (void)unused;
    while (g_timer_run) {
        Sleep(50);
        audio_collect_finished();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Channel helpers                                                    */
/* ------------------------------------------------------------------ */
static void channel_reset(int ch)
{
    g_ch[ch].program    = 0;
    g_ch[ch].bank_msb   = 0;
    g_ch[ch].bank_lsb   = 0;
    g_ch[ch].pan        = 64;
    g_ch[ch].volume     = 100;
    g_ch[ch].expression = 127;
    g_ch[ch].reverb     = 0;
    g_ch[ch].chorus     = 0;
}

static EffectSends channel_sends(int ch)
{
    EffectSends s;
    s.reverb_send = (float)g_ch[ch].reverb / 127.0f;
    s.chorus_send = (float)g_ch[ch].chorus / 127.0f;
    return s;
}

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
static void midi_note_off(int ch, int note);
static void midi_all_notes_off(int ch);

/* ------------------------------------------------------------------ */
/*  Note slot management                                               */
/* ------------------------------------------------------------------ */
static int find_note_slot(int ch, int note)
{
    for (int i = 0; i < MAX_NOTE_SLOTS; i++) {
        if (g_note_slots[i].used &&
            g_note_slots[i].channel == ch &&
            g_note_slots[i].note    == note)
            return i;
    }
    return -1;
}

static int alloc_note_slot(void)
{
    for (int i = 0; i < MAX_NOTE_SLOTS; i++) {
        if (!g_note_slots[i].used) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  MIDI event handlers                                                */
/* ------------------------------------------------------------------ */
static void midi_note_on(int ch, int note, int velocity)
{
    if (velocity == 0) { midi_note_off(ch, note); return; }

    LOG("midi_note_on: ch=%d note=%d vel=%d prog=%d pan=%d vol=%d",
        ch, note, velocity, g_ch[ch].program, g_ch[ch].pan, g_ch[ch].volume);

    float x, y, z;
    audio_note_position(g_ch[ch].pan, note, g_ch[ch].volume, &x, &y, &z);

    EffectSends sends = channel_sends(ch);
    int bank = (g_ch[ch].bank_msb << 7) | g_ch[ch].bank_lsb;
    int vi   = -1;

    if (g_sftype == SF_SF2 && g_sf2) {
        SF2Zone zone;
        if (sf2_find_zone(g_sf2, bank, g_ch[ch].program, note, velocity, &zone)) {
            vi = audio_note_on_sf2(note, g_sf2, &zone, x, y, z, &sends);
        }
    } else if (g_sftype == SF_SFZ && g_sfz) {
        SFZRegion *r = sfz_find_region(g_sfz, note, velocity);
        if (r) {
            vi = audio_note_on_sfz(note, g_sfz, r, x, y, z, &sends);
        }
    }

    if (vi < 0) return;

    EnterCriticalSection(&g_note_cs);
    int si = alloc_note_slot();
    if (si >= 0) {
        g_note_slots[si].used         = 1;
        g_note_slots[si].channel      = ch;
        g_note_slots[si].note         = note;
        g_note_slots[si].voice_handle = vi;
    } else {
        LOG("midi_note_on: note slot pool exhausted – killing voice");
        audio_note_kill(vi);
    }
    LeaveCriticalSection(&g_note_cs);
}

static void midi_note_off(int ch, int note)
{
    LOG("midi_note_off: ch=%d note=%d", ch, note);
    EnterCriticalSection(&g_note_cs);
    int si = find_note_slot(ch, note);
    if (si >= 0) {
        audio_note_off(g_note_slots[si].voice_handle);
        g_note_slots[si].used = 0;
    }
    LeaveCriticalSection(&g_note_cs);
}

static void midi_all_notes_off(int ch)
{
    LOG("midi_all_notes_off: ch=%d", ch);
    EnterCriticalSection(&g_note_cs);
    for (int i = 0; i < MAX_NOTE_SLOTS; i++) {
        if (g_note_slots[i].used && g_note_slots[i].channel == ch) {
            audio_note_off(g_note_slots[i].voice_handle);
            g_note_slots[i].used = 0;
        }
    }
    LeaveCriticalSection(&g_note_cs);
}

static void midi_all_sound_off(int ch)
{
    LOG("midi_all_sound_off: ch=%d", ch);
    EnterCriticalSection(&g_note_cs);
    for (int i = 0; i < MAX_NOTE_SLOTS; i++) {
        if (g_note_slots[i].used && g_note_slots[i].channel == ch) {
            audio_note_kill(g_note_slots[i].voice_handle);
            g_note_slots[i].used = 0;
        }
    }
    LeaveCriticalSection(&g_note_cs);
}

static void midi_control_change(int ch, int ctrl, int value)
{
    LOG("midi_cc: ch=%d ctrl=%d val=%d", ch, ctrl, value);
    switch (ctrl) {
        case 0:   g_ch[ch].bank_msb   = value; break;
        case 7:   g_ch[ch].volume     = value; break;
        case 10:  g_ch[ch].pan        = value; break;
        case 11:  g_ch[ch].expression = value; break;
        case 32:  g_ch[ch].bank_lsb   = value; break;

        case 91:  /* Reverb Depth */
            g_ch[ch].reverb = value;
            audio_set_reverb_level((float)value / 127.0f);
            break;

        case 93:  /* Chorus Depth */
            g_ch[ch].chorus = value;
            audio_set_chorus_level((float)value / 127.0f);
            break;

        case 120: /* All Sound Off */
            midi_all_sound_off(ch);
            break;
        case 121: /* Reset All Controllers */
            channel_reset(ch);
            break;
        case 123: /* All Notes Off */
        case 124: case 125: case 126: case 127:
            midi_all_notes_off(ch);
            break;
    }
}

static void midi_program_change(int ch, int prog)
{
    LOG("midi_program_change: ch=%d prog=%d", ch, prog);
    g_ch[ch].program = prog & 0x7F;
}

static void midi_pitch_bend(int ch, int lsb, int msb)
{
    /* Pitch bend not applied to already-playing sources in this version */
    (void)ch; (void)lsb; (void)msb;
}

/* ------------------------------------------------------------------ */
/*  Process a short MIDI message (3 bytes packed in DWORD)             */
/* ------------------------------------------------------------------ */
static void process_short_msg(DWORD msg)
{
    uint8_t status = (uint8_t)(msg & 0xFF);
    uint8_t d1     = (uint8_t)((msg >> 8)  & 0x7F);
    uint8_t d2     = (uint8_t)((msg >> 16) & 0x7F);

    uint8_t type = status & 0xF0;
    uint8_t ch   = status & 0x0F;

    switch (type) {
        case 0x80: midi_note_off      (ch, d1);        break;
        case 0x90: midi_note_on       (ch, d1, d2);    break;
        case 0xA0: /* aftertouch – ignore */            break;
        case 0xB0: midi_control_change(ch, d1, d2);    break;
        case 0xC0: midi_program_change(ch, d1);         break;
        case 0xD0: /* channel pressure – ignore */      break;
        case 0xE0: midi_pitch_bend    (ch, d1, d2);    break;
        case 0xF0:
            /* System messages */
            if (status == 0xFF) { /* MIDI reset */
                audio_all_notes_off();
                for (int i = 0; i < MIDI_CHANNELS; i++) channel_reset(i);
            }
            break;
    }
}

/* ------------------------------------------------------------------ */
/*  Driver initialisation / teardown                                   */
/* ------------------------------------------------------------------ */
static int        g_open_count = 0;
static CRITICAL_SECTION g_open_cs;

/* Callback info saved from MIDIOPENDESC during MODM_OPEN */
static HMIDIOUT   g_hmidi        = NULL;
static DWORD_PTR  g_callback     = 0;
static DWORD_PTR  g_callback_inst= 0;
static DWORD      g_callback_flags = 0;

/* Send a WinMM driver callback (MOM_OPEN, MOM_CLOSE, MOM_DONE …) */
static void send_callback(DWORD uMsg, DWORD_PTR p1, DWORD_PTR p2)
{
    if (!g_callback) return;
    DriverCallback(g_callback,
                   g_callback_flags,
                   (HDRVR)g_hmidi,
                   uMsg,
                   g_callback_inst,
                   p1,
                   p2);
}

static DWORD driver_open(MIDIOPENDESC *mod, DWORD flags)
{
    EnterCriticalSection(&g_open_cs);

    /* Save WinMM callback info (only the first caller matters) */
    if (mod) {
        g_hmidi         = mod->hMidi;
        g_callback      = mod->dwCallback;
        g_callback_inst = mod->dwInstance;
        /* Flags from modMessage dwParam2 carry DCB_* bits */
        g_callback_flags = flags & CALLBACK_TYPEMASK;
    }

    if (g_open_count++ == 0) {
        log_init();
        LOG(MI3DI_FULL_VERSION " – driver opened");

        for (int i = 0; i < MIDI_CHANNELS; i++) channel_reset(i);
        InitializeCriticalSection(&g_note_cs);
        memset(g_note_slots, 0, sizeof(g_note_slots));

        if (!audio_init()) {
            LOG("driver_open: audio_init failed");
            g_open_count = 0;
            LeaveCriticalSection(&g_open_cs);
            return MMSYSERR_ERROR;
        }

        load_soundfont();

        g_timer_run = 1;
        g_timer_thread = CreateThread(NULL, 0, timer_thread_proc,
                                      NULL, 0, NULL);
    }
    LeaveCriticalSection(&g_open_cs);

    send_callback(MOM_OPEN, 0, 0);
    return MMSYSERR_NOERROR;
}

static DWORD driver_close(void)
{
    EnterCriticalSection(&g_open_cs);
    if (--g_open_count <= 0) {
        g_open_count = 0;
        g_timer_run  = 0;
        if (g_timer_thread) {
            WaitForSingleObject(g_timer_thread, 500);
            CloseHandle(g_timer_thread);
            g_timer_thread = NULL;
        }

        audio_all_notes_off();
        audio_shutdown();

        DeleteCriticalSection(&g_note_cs);

        if (g_sf2) { sf2_free(g_sf2); g_sf2 = NULL; }
        if (g_sfz) { sfz_free(g_sfz); g_sfz = NULL; }
        g_sftype = SF_NONE;

        LOG(MI3DI_FULL_VERSION " – driver closed");
        log_close();
    }
    LeaveCriticalSection(&g_open_cs);

    send_callback(MOM_CLOSE, 0, 0);
    return MMSYSERR_NOERROR;
}

/* ------------------------------------------------------------------ */
/*  modMessage – required WinMM MIDI output driver entry point         */
/* ------------------------------------------------------------------ */
DWORD WINAPI modMessage(UINT  uDeviceID,
                        UINT  uMsg,
                        DWORD_PTR dwUser,
                        DWORD_PTR dwParam1,
                        DWORD_PTR dwParam2)
{
    (void)uDeviceID;
    (void)dwUser;   /* callback info is stored during MODM_OPEN via MIDIOPENDESC */

    switch (uMsg) {
        case MODM_GETNUMDEVS:
            return 1;

        case MODM_GETDEVCAPS: {
            MIDIOUTCAPSA *caps = (MIDIOUTCAPSA *)(void *)dwParam1;
            if (!caps) return MMSYSERR_INVALPARAM;
            memset(caps, 0, sizeof(*caps));
            /* 0xFFFF = MM_UNMAPPED / MM_PID_UNMAPPED – use raw values for
             * portability; the constants are absent from some MinGW headers */
            caps->wMid         = 0xFFFFu;
            caps->wPid         = 0xFFFFu;
            caps->vDriverVersion = MAKELONG(MI3DI_VERSION_MINOR, MI3DI_VERSION_MAJOR);
            strncpy(caps->szPname, "MI3DI 3D MIDI Synth", MAXPNAMELEN - 1);
            caps->wTechnology  = MOD_SYNTH;
            caps->wVoices      = MI3DI_MAX_VOICES;
            caps->wNotes       = MI3DI_MAX_VOICES;
            caps->wChannelMask = 0xFFFF;
            caps->dwSupport    = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME;
            return MMSYSERR_NOERROR;
        }

        case MODM_OPEN:
            return driver_open((MIDIOPENDESC *)(void *)dwParam1, (DWORD)dwParam2);

        case MODM_CLOSE:
            return driver_close();

        case MODM_DATA:
            process_short_msg((DWORD)dwParam1);
            return MMSYSERR_NOERROR;

        case MODM_LONGDATA: {
            MIDIHDR *hdr = (MIDIHDR *)(void *)dwParam1;
            if (!hdr) return MMSYSERR_INVALPARAM;
            /* SysEx – acknowledge buffer completion to the caller */
            hdr->dwFlags |= MHDR_DONE;
            send_callback(MOM_DONE, dwParam1, 0);
            return MMSYSERR_NOERROR;
        }

        case MODM_PREPARE:   return MMSYSERR_NOTSUPPORTED;
        case MODM_UNPREPARE: return MMSYSERR_NOTSUPPORTED;

        case MODM_RESET:
            audio_all_notes_off();
            for (int i = 0; i < MIDI_CHANNELS; i++) channel_reset(i);
            return MMSYSERR_NOERROR;

        case MODM_GETVOLUME:
            if (dwParam1) *(DWORD *)(void *)dwParam1 = 0xFFFFFFFF;
            return MMSYSERR_NOERROR;

        case MODM_SETVOLUME:
            /* Volume managed by 3D distance model */
            return MMSYSERR_NOERROR;

        default:
            return MMSYSERR_NOTSUPPORTED;
    }
}

/* ------------------------------------------------------------------ */
/*  DriverProc – required WinMM driver entry point                     */
/* ------------------------------------------------------------------ */
LONG WINAPI DriverProc(DWORD_PTR dwDriverId,
                       HDRVR     hdrvr,
                       UINT      uMsg,
                       LONG      lParam1,
                       LONG      lParam2)
{
    (void)dwDriverId; (void)hdrvr; (void)lParam1; (void)lParam2;

    switch (uMsg) {
        case DRV_LOAD:
            InitializeCriticalSection(&g_open_cs);
            return 1;
        case DRV_FREE:
            DeleteCriticalSection(&g_open_cs);
            return 1;
        case DRV_OPEN:
        case DRV_CLOSE:
        case DRV_ENABLE:
        case DRV_DISABLE:
            return 1;
        case DRV_QUERYCONFIGURE:
            return 0; /* No configuration dialog */
        case DRV_CONFIGURE:
            return DRVCNF_CANCEL;
        case DRV_INSTALL:
        case DRV_REMOVE:
            return DRVCNF_OK;
    }
    return DefDriverProc(dwDriverId, hdrvr, uMsg, lParam1, lParam2);
}

/* ------------------------------------------------------------------ */
/*  DllMain                                                            */
/* ------------------------------------------------------------------ */
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)hInst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hInst);
    return TRUE;
}
