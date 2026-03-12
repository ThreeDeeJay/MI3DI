/*
 * MI3DI – SF2 (SoundFont 2) parser
 *
 * Parses the RIFF-based SF2 format, builds in-memory tables of
 * presets / instruments / samples, and provides a zone-lookup that
 * returns everything needed to play a single note via OpenAL.
 *
 * References: SoundFont® 2.04 specification
 */
#include "sf2.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  RIFF helpers                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE    *f;
    long     base;   /* file offset of this chunk's data start        */
    uint32_t size;   /* data size (not including 8-byte header)        */
    uint32_t id;
    uint32_t type;   /* only valid for LIST / RIFF chunks              */
} RiffChunk;

static uint32_t fourcc(const char *s)
{
    return (uint32_t)((uint8_t)s[0]       |
                      (uint8_t)s[1] <<  8 |
                      (uint8_t)s[2] << 16 |
                      (uint8_t)s[3] << 24);
}

/* Read next chunk header; leaves file pointer at start of chunk data */
static int riff_next(FILE *f, long end, RiffChunk *c)
{
    long pos = ftell(f);
    if (pos + 8 > end) return 0;

    if (fread(&c->id,   4, 1, f) != 1) return 0;
    if (fread(&c->size, 4, 1, f) != 1) return 0;

    c->f    = f;
    c->base = ftell(f);
    c->type = 0;

    if (c->id == fourcc("RIFF") || c->id == fourcc("LIST")) {
        if (fread(&c->type, 4, 1, f) != 1) return 0;
    }
    return 1;
}

/* Skip to the next chunk after the current one */
static void riff_skip(RiffChunk *c)
{
    long next = c->base + c->size;
    if (next & 1) next++; /* RIFF chunks are word-aligned */
    fseek(c->f, next, SEEK_SET);
}

/* Seek to a named sub-chunk inside a LIST.  Returns 1 on success. */
static int riff_find(FILE *f, long parent_base, long parent_end,
                     const char *id, RiffChunk *out)
{
    fseek(f, parent_base, SEEK_SET);
    while (1) {
        if (!riff_next(f, parent_end, out)) return 0;
        if (out->id == fourcc(id))          return 1;
        riff_skip(out);
    }
}

/* ------------------------------------------------------------------ */
/*  Generic chunk loader (allocates buffer)                            */
/* ------------------------------------------------------------------ */
static void *load_chunk(FILE *f, long base, uint32_t size)
{
    void *buf = malloc(size);
    if (!buf) return NULL;
    fseek(f, base, SEEK_SET);
    if (fread(buf, 1, size, f) != size) { free(buf); return NULL; }
    return buf;
}

/* ------------------------------------------------------------------ */
/*  sf2_load                                                           */
/* ------------------------------------------------------------------ */
SF2 *sf2_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG("sf2_load: cannot open '%s'", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Top-level RIFF 'sfbk' */
    RiffChunk riff;
    if (!riff_next(f, fsize, &riff) ||
        riff.id   != fourcc("RIFF") ||
        riff.type != fourcc("sfbk"))
    {
        LOG("sf2_load: not a valid SF2 file");
        fclose(f); return NULL;
    }

    long sfbk_end = riff.base + riff.size;

    /* Find LIST 'sdta' (sample data) */
    RiffChunk sdta;
    if (!riff_find(f, riff.base, sfbk_end, "LIST", &sdta) ||
        sdta.type != fourcc("sdta"))
    {
        LOG("sf2_load: missing sdta chunk");
        fclose(f); return NULL;
    }
    long sdta_end = sdta.base + sdta.size;

    RiffChunk smpl;
    if (!riff_find(f, sdta.base + 4, sdta_end, "smpl", &smpl)) {
        LOG("sf2_load: missing smpl chunk");
        fclose(f); return NULL;
    }

    /* Find LIST 'pdta' (preset data) */
    RiffChunk pdta;
    fseek(f, riff.base, SEEK_SET);
    /* Need to find pdta by scanning from the start of sfbk */
    {
        long scan = riff.base;
        int found = 0;
        while (scan < sfbk_end) {
            RiffChunk c;
            fseek(f, scan, SEEK_SET);
            if (!riff_next(f, sfbk_end, &c)) break;
            if (c.id == fourcc("LIST") && c.type == fourcc("pdta")) {
                pdta  = c;
                found = 1;
                break;
            }
            riff_skip(&c);
            scan = ftell(f);
        }
        if (!found) {
            LOG("sf2_load: missing pdta chunk");
            fclose(f); return NULL;
        }
    }
    long pdta_end = pdta.base + pdta.size;

    /* Helper to find a pdta sub-chunk */
#define FIND_PDTA(name, chunk) \
    riff_find(f, pdta.base + 4, pdta_end, name, &(chunk))

    RiffChunk cPHDR, cPBAG, cPGEN, cINST, cIBAG, cIGEN, cSHDR;
    if (!FIND_PDTA("PHDR", cPHDR)) { LOG("sf2_load: no PHDR"); fclose(f); return NULL; }
    if (!FIND_PDTA("PBAG", cPBAG)) { LOG("sf2_load: no PBAG"); fclose(f); return NULL; }
    if (!FIND_PDTA("PGEN", cPGEN)) { LOG("sf2_load: no PGEN"); fclose(f); return NULL; }
    if (!FIND_PDTA("INST", cINST)) { LOG("sf2_load: no INST"); fclose(f); return NULL; }
    if (!FIND_PDTA("IBAG", cIBAG)) { LOG("sf2_load: no IBAG"); fclose(f); return NULL; }
    if (!FIND_PDTA("IGEN", cIGEN)) { LOG("sf2_load: no IGEN"); fclose(f); return NULL; }
    if (!FIND_PDTA("SHDR", cSHDR)) { LOG("sf2_load: no SHDR"); fclose(f); return NULL; }
#undef FIND_PDTA

    SF2 *sf = (SF2 *)calloc(1, sizeof(SF2));
    if (!sf) { fclose(f); return NULL; }

    /* Load each table */
    sf->presets       = (sfPresetHeader *)load_chunk(f, cPHDR.base, cPHDR.size);
    sf->presetCount   = cPHDR.size / sizeof(sfPresetHeader);

    sf->presetBags    = (sfBag *)load_chunk(f, cPBAG.base, cPBAG.size);
    sf->presetBagCount= cPBAG.size / sizeof(sfBag);

    sf->presetGens    = (sfGenList *)load_chunk(f, cPGEN.base, cPGEN.size);
    sf->presetGenCount= cPGEN.size / sizeof(sfGenList);

    sf->instruments       = (sfInst *)load_chunk(f, cINST.base, cINST.size);
    sf->instrumentCount   = cINST.size / sizeof(sfInst);

    sf->instBags      = (sfBag *)load_chunk(f, cIBAG.base, cIBAG.size);
    sf->instBagCount  = cIBAG.size / sizeof(sfBag);

    sf->instGens      = (sfGenList *)load_chunk(f, cIGEN.base, cIGEN.size);
    sf->instGenCount  = cIGEN.size / sizeof(sfGenList);

    sf->sampleHeaders = (sfSample *)load_chunk(f, cSHDR.base, cSHDR.size);
    sf->sampleCount   = cSHDR.size / sizeof(sfSample);

    /* Sample data (16-bit PCM) */
    sf->sampleDataLen = smpl.size / sizeof(int16_t);
    sf->sampleData    = (int16_t *)load_chunk(f, smpl.base, smpl.size);

    fclose(f);

    if (!sf->presets || !sf->presetBags || !sf->presetGens ||
        !sf->instruments || !sf->instBags || !sf->instGens ||
        !sf->sampleHeaders || !sf->sampleData)
    {
        LOG("sf2_load: allocation failure");
        sf2_free(sf);
        return NULL;
    }

    LOG("sf2_load: loaded '%s'  presets=%d  samples=%d  pcm_frames=%u",
        path, sf->presetCount, sf->sampleCount, sf->sampleDataLen);
    return sf;
}

void sf2_free(SF2 *sf)
{
    if (!sf) return;
    free(sf->presets);
    free(sf->presetBags);
    free(sf->presetGens);
    free(sf->instruments);
    free(sf->instBags);
    free(sf->instGens);
    free(sf->sampleHeaders);
    free(sf->sampleData);
    free(sf);
}

/* ------------------------------------------------------------------ */
/*  Generator scanning helpers                                         */
/* ------------------------------------------------------------------ */

/* Scan a generator list for an opcode; return the sfGenAmount or NULL */
static sfGenAmount *find_gen(sfGenList *gens, int first, int last, int oper)
{
    for (int i = first; i < last; i++) {
        if (gens[i].sfGenOper == (uint16_t)oper)
            return &gens[i].genAmount;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  sf2_find_zone                                                      */
/* ------------------------------------------------------------------ */
int sf2_find_zone(SF2 *sf, int bank, int program,
                  int note, int velocity,
                  SF2Zone *out)
{
    if (!sf || !out) return 0;

    /* Default generator values */
    memset(out, 0, sizeof(*out));
    out->sampleIdx        = -1;
    out->scaleTuning      = 100;
    out->overridingRootKey= -1;
    out->sampleModes      = SF_SAMPLE_NO_LOOP;

    /* ---- 1. Find preset by bank/program ---- */
    int presetIdx = -1;
    for (int p = 0; p < sf->presetCount - 1; p++) {
        if (sf->presets[p].wBank   == (uint16_t)bank &&
            sf->presets[p].wPreset == (uint16_t)program)
        {
            presetIdx = p;
            break;
        }
    }
    if (presetIdx < 0) {
        /* Fall back to bank 0 same program */
        for (int p = 0; p < sf->presetCount - 1; p++) {
            if (sf->presets[p].wBank   == 0 &&
                sf->presets[p].wPreset == (uint16_t)program)
            {
                presetIdx = p;
                break;
            }
        }
    }
    if (presetIdx < 0) {
        LOG("sf2_find_zone: no preset for bank=%d prog=%d", bank, program);
        return 0;
    }

    /* ---- 2. Iterate preset zones (bags) ---- */
    int pbag_start = sf->presets[presetIdx].wPresetBagNdx;
    int pbag_end   = sf->presets[presetIdx + 1].wPresetBagNdx;

    for (int pb = pbag_start; pb < pbag_end; pb++) {
        int pg_start = sf->presetBags[pb].wGenNdx;
        int pg_end   = sf->presetBags[pb + 1].wGenNdx;

        /* Check key/vel range for this preset zone */
        sfGenAmount *kr = find_gen(sf->presetGens, pg_start, pg_end, SF_GEN_KEY_RANGE);
        sfGenAmount *vr = find_gen(sf->presetGens, pg_start, pg_end, SF_GEN_VEL_RANGE);

        if (kr && (note < kr->range.lo || note > kr->range.hi)) continue;
        if (vr && (velocity < vr->range.lo || velocity > vr->range.hi)) continue;

        /* Get instrument index */
        sfGenAmount *ig = find_gen(sf->presetGens, pg_start, pg_end, SF_GEN_INSTRUMENT);
        if (!ig) continue;
        int instIdx = ig->wAmount;
        if (instIdx >= sf->instrumentCount - 1) continue;

        /* ---- 3. Iterate instrument zones ---- */
        int ibag_start = sf->instruments[instIdx].wInstBagNdx;
        int ibag_end   = sf->instruments[instIdx + 1].wInstBagNdx;

        int global_ig_start = -1, global_ig_end = -1;

        for (int ib = ibag_start; ib < ibag_end; ib++) {
            int igen_start = sf->instBags[ib].wInstGenNdx;
            int igen_end   = sf->instBags[ib + 1].wInstGenNdx;

            /* Detect global zone: first zone without sampleID */
            sfGenAmount *sid = find_gen(sf->instGens, igen_start, igen_end, SF_GEN_SAMPLE_ID);
            if (!sid) {
                if (ib == ibag_start) {
                    global_ig_start = igen_start;
                    global_ig_end   = igen_end;
                }
                continue;
            }

            /* Check key/vel range */
            sfGenAmount *ikr = find_gen(sf->instGens, igen_start, igen_end, SF_GEN_KEY_RANGE);
            sfGenAmount *ivr = find_gen(sf->instGens, igen_start, igen_end, SF_GEN_VEL_RANGE);

            if (ikr && (note < ikr->range.lo || note > ikr->range.hi)) continue;
            if (ivr && (velocity < ivr->range.lo || velocity > ivr->range.hi)) continue;

            /* ---- Found matching instrument zone ---- */

            /* Helper: read gen from instrument zone, falling back to global */
#define GET_IGEN(oper) \
    (find_gen(sf->instGens, igen_start, igen_end, (oper)) ? \
     find_gen(sf->instGens, igen_start, igen_end, (oper)) : \
     (global_ig_start >= 0 ? find_gen(sf->instGens, global_ig_start, global_ig_end, (oper)) : NULL))

            sfGenAmount *g;

            out->sampleIdx = sid->wAmount;

            if ((g = GET_IGEN(SF_GEN_COARSE_TUNE)))       out->coarseTune        = g->shAmount;
            if ((g = GET_IGEN(SF_GEN_FINE_TUNE)))         out->fineTune          = g->shAmount;
            if ((g = GET_IGEN(SF_GEN_SCALE_TUNING)))      out->scaleTuning       = g->shAmount;
            if ((g = GET_IGEN(SF_GEN_OVERRIDING_ROOT_KEY))) {
                if (g->shAmount != -1) out->overridingRootKey = g->shAmount;
            }
            if ((g = GET_IGEN(SF_GEN_SAMPLE_MODES)))      out->sampleModes       = g->shAmount & 3;
            if ((g = GET_IGEN(SF_GEN_INIT_ATTENUATION)))  out->initialAttenuation= g->shAmount;
            if ((g = GET_IGEN(SF_GEN_PAN)))               out->pan               = g->shAmount;

            if ((g = GET_IGEN(SF_GEN_START_ADDRS_OFFSET)))  out->startAddrsOffset  = g->shAmount;
            if ((g = GET_IGEN(SF_GEN_END_ADDRS_OFFSET)))    out->endAddrsOffset    = g->shAmount;
            if ((g = GET_IGEN(SF_GEN_STARTLOOP_ADDRS_OFFSET))) out->startloopAddrsOffset = g->shAmount;
            if ((g = GET_IGEN(SF_GEN_ENDLOOP_ADDRS_OFFSET)))   out->endloopAddrsOffset   = g->shAmount;

            /* Coarse address offsets (in 32768-sample units) */
            sfGenAmount *sac, *eac, *slac, *elac;
            if ((sac  = GET_IGEN(SF_GEN_START_ADDRS_COARSE)))
                out->startAddrsOffset     += (int32_t)sac->shAmount  * 32768;
            if ((eac  = GET_IGEN(12 /*endAddrsCoarseOffset*/)))
                out->endAddrsOffset       += (int32_t)eac->shAmount  * 32768;
            if ((slac = GET_IGEN(SF_GEN_STARTLOOP_ADDRS_COARSE)))
                out->startloopAddrsOffset += (int32_t)slac->shAmount * 32768;
            if ((elac = GET_IGEN(51 /*endloopAddrsCoarseOffset*/)))
                out->endloopAddrsOffset   += (int32_t)elac->shAmount * 32768;

#undef GET_IGEN

            LOG("sf2_find_zone: found zone for note=%d prog=%d "
                "sampleIdx=%d loop=%d rootKey=%d",
                note, program, out->sampleIdx,
                out->sampleModes, out->overridingRootKey);
            return 1;
        }
    }

    LOG("sf2_find_zone: no zone matched note=%d vel=%d", note, velocity);
    return 0;
}
