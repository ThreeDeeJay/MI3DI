/*
 * MI3DI – SFZ parser
 *
 * Supports the most common SFZ 1.0 opcodes required for basic
 * playback: sample, lokey/hikey, lovel/hivel, pitch_keycenter,
 * tune, transpose, loop_mode, loop_start, loop_end, volume, pan.
 */
#include "sfz.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/*  Tiny string helpers                                                 */
/* ------------------------------------------------------------------ */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

/* Convert a MIDI note name (e.g. "C4", "A#3") to a number */
static int note_name_to_num(const char *s)
{
    static const char *names = "C D EF G A B";
    int len = (int)strlen(s);
    if (len < 2) return -1;

    char upper = (char)toupper((unsigned char)s[0]);
    const char *p = strchr(names, upper);
    if (!p) return -1;
    int semitone = (int)(p - names);

    int idx = 1;
    int sharp = 0;
    if (s[idx] == '#') { sharp = 1; idx++; }
    else if (s[idx] == 'b') { sharp = -1; idx++; }

    if (!isdigit((unsigned char)s[idx])) return -1;
    int octave = atoi(s + idx);

    return (octave + 1) * 12 + semitone + sharp;
}

/* Parse a key value that may be a number or a note name */
static int parse_key(const char *val)
{
    if (isdigit((unsigned char)*val) || *val == '-')
        return atoi(val);
    return note_name_to_num(val);
}

/* ------------------------------------------------------------------ */
/*  sfz_load                                                           */
/* ------------------------------------------------------------------ */
SFZ *sfz_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG("sfz_load: cannot open '%s'", path);
        return NULL;
    }

    SFZ *sfz = (SFZ *)calloc(1, sizeof(SFZ));
    if (!sfz) { fclose(f); return NULL; }

    /* Extract base directory */
    strncpy(sfz->basedir, path, SFZ_MAX_PATH - 1);
    char *sep = strrchr(sfz->basedir, '\\');
    if (!sep) sep = strrchr(sfz->basedir, '/');
    if (sep) sep[1] = '\0';
    else     sfz->basedir[0] = '\0';

    /* Capacity-based growing array */
    int capacity = 16;
    sfz->regions = (SFZRegion *)malloc(capacity * sizeof(SFZRegion));
    if (!sfz->regions) { fclose(f); free(sfz); return NULL; }

    /* Default region prototype */
    SFZRegion proto;
    memset(&proto, 0, sizeof(proto));
    proto.lokey  = 0;
    proto.hikey  = 127;
    proto.lovel  = 1;
    proto.hivel  = 127;
    proto.pitch_keycenter = 60;
    proto.loop_mode       = 0;

    SFZRegion cur;
    int in_region = 0;
    int in_group  = 0;
    SFZRegion group_proto;
    memcpy(&group_proto, &proto, sizeof(proto));

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Strip comments */
        char *comment = strstr(line, "//");
        if (comment) *comment = '\0';

        char *l = trim(line);
        if (!*l) continue;

        /* Section headers */
        if (*l == '<') {
            char *end = strchr(l, '>');
            if (!end) continue;
            char tag[32];
            int tlen = (int)(end - l - 1);
            if (tlen >= (int)sizeof(tag)) tlen = (int)sizeof(tag) - 1;
            memcpy(tag, l + 1, tlen);
            tag[tlen] = '\0';

            if (strcmp(tag, "region") == 0) {
                /* Commit previous region if any */
                if (in_region && cur.sample[0]) {
                    if (sfz->count >= capacity) {
                        capacity *= 2;
                        sfz->regions = (SFZRegion *)realloc(sfz->regions,
                                            capacity * sizeof(SFZRegion));
                    }
                    memcpy(&sfz->regions[sfz->count++], &cur, sizeof(cur));
                }
                /* Start new region from group defaults */
                memcpy(&cur, &group_proto, sizeof(cur));
                in_region = 1;
                l = end + 1; /* continue parsing opcodes on same line */
            } else if (strcmp(tag, "group") == 0) {
                if (in_region && cur.sample[0]) {
                    if (sfz->count >= capacity) {
                        capacity *= 2;
                        sfz->regions = (SFZRegion *)realloc(sfz->regions,
                                            capacity * sizeof(SFZRegion));
                    }
                    memcpy(&sfz->regions[sfz->count++], &cur, sizeof(cur));
                }
                memcpy(&group_proto, &proto, sizeof(proto));
                in_region = 0;
                in_group  = 1;
                l = end + 1;
            } else if (strcmp(tag, "control") == 0 || strcmp(tag, "global") == 0) {
                in_region = 0;
                in_group  = 0;
                continue;
            } else {
                l = end + 1;
            }
        }

        /* Parse opcodes: key=value pairs separated by whitespace */
        SFZRegion *target = in_region ? &cur : (in_group ? &group_proto : NULL);

        char *tok = l;
        while (*tok) {
            /* skip whitespace */
            while (*tok && isspace((unsigned char)*tok)) tok++;
            if (!*tok) break;

            /* find '=' */
            char *eq = strchr(tok, '=');
            if (!eq) break;

            /* key name */
            char key[64];
            int klen = (int)(eq - tok);
            if (klen >= (int)sizeof(key)) klen = (int)sizeof(key) - 1;
            memcpy(key, tok, klen);
            key[klen] = '\0';
            /* trim key */
            char *kk = trim(key);

            /* value: up to next whitespace OR end of line.
             * Exception: "sample" value may contain spaces (rare but possible) */
            char *vstart = eq + 1;
            while (*vstart && isspace((unsigned char)*vstart)) vstart++;

            char val[SFZ_MAX_PATH];
            if (strcmp(kk, "sample") == 0) {
                /* read rest of token until next opcode (next 'word=') */
                /* Simple heuristic: take everything to end of line */
                strncpy(val, vstart, SFZ_MAX_PATH - 1);
                val[SFZ_MAX_PATH - 1] = '\0';
                /* strip trailing whitespace */
                char *ve = val + strlen(val);
                while (ve > val && isspace((unsigned char)ve[-1])) ve--;
                *ve = '\0';
                tok = vstart + strlen(vstart);
            } else {
                char *vend = vstart;
                while (*vend && !isspace((unsigned char)*vend)) vend++;
                int vlen = (int)(vend - vstart);
                if (vlen >= (int)sizeof(val)) vlen = (int)sizeof(val) - 1;
                memcpy(val, vstart, vlen);
                val[vlen] = '\0';
                tok = vend;
            }

            if (!target) continue;

            /* Apply opcode */
            if      (strcmp(kk, "sample")           == 0) {
                /* Normalise path separators */
                target->sample[SFZ_MAX_PATH - 1] = '\0';
                strncpy(target->sample, val, SFZ_MAX_PATH - 1);
                for (char *p = target->sample; *p; p++)
                    if (*p == '/') *p = '\\';
            }
            else if (strcmp(kk, "lokey")            == 0) target->lokey  = parse_key(val);
            else if (strcmp(kk, "hikey")            == 0) target->hikey  = parse_key(val);
            else if (strcmp(kk, "key")              == 0) {
                target->lokey = target->hikey = target->pitch_keycenter = parse_key(val);
            }
            else if (strcmp(kk, "lovel")            == 0) target->lovel  = atoi(val);
            else if (strcmp(kk, "hivel")            == 0) target->hivel  = atoi(val);
            else if (strcmp(kk, "pitch_keycenter")  == 0) target->pitch_keycenter = parse_key(val);
            else if (strcmp(kk, "tune")             == 0) target->tune        = (float)atof(val);
            else if (strcmp(kk, "transpose")        == 0) target->transpose   = atoi(val);
            else if (strcmp(kk, "loop_mode")        == 0) {
                if      (strcmp(val, "loop_continuous") == 0) target->loop_mode = 1;
                else if (strcmp(val, "one_shot")        == 0) target->loop_mode = 2;
                else                                          target->loop_mode = 0;
            }
            else if (strcmp(kk, "loop_start")       == 0) target->loop_start = atoi(val);
            else if (strcmp(kk, "loop_end")         == 0) target->loop_end   = atoi(val);
            else if (strcmp(kk, "volume")           == 0) target->volume = (float)atof(val);
            else if (strcmp(kk, "pan")              == 0) target->pan    = (float)atof(val);
        }
    }

    /* Commit final region */
    if (in_region && cur.sample[0]) {
        if (sfz->count >= capacity) {
            capacity++;
            sfz->regions = (SFZRegion *)realloc(sfz->regions,
                                capacity * sizeof(SFZRegion));
        }
        memcpy(&sfz->regions[sfz->count++], &cur, sizeof(cur));
    }

    fclose(f);
    LOG("sfz_load: loaded '%s'  regions=%d", path, sfz->count);
    return sfz;
}

void sfz_free(SFZ *sfz)
{
    if (!sfz) return;
    free(sfz->regions);
    free(sfz);
}

SFZRegion *sfz_find_region(SFZ *sfz, int note, int velocity)
{
    if (!sfz) return NULL;
    for (int i = 0; i < sfz->count; i++) {
        SFZRegion *r = &sfz->regions[i];
        if (note     >= r->lokey && note     <= r->hikey &&
            velocity >= r->lovel && velocity <= r->hivel)
            return r;
    }
    return NULL;
}
