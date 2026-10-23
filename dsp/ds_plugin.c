/*
 * Deep Space Convolution - Schwung audio FX v2 adapter.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * Every entry point here runs on Move's SPI callback (or, for a bus insert,
 * the bus worker): no allocation, no file I/O, no locks. Instances come from
 * a static pool claimed with an atomic compare-and-swap, so even
 * create_instance allocates nothing; a never-used slot is already zero, a
 * reused one is cleared a slice per block by the engine itself.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ds_engine.h"
#include "ds_metadata.h"
#include "host/audio_fx_api_v2.h"

#define DS_POOL 8

typedef struct {
    int used;
    int dirty;   /* has held an instance before: its memory is not zero */
    ds_engine engine;
} ds_slot;

static ds_slot g_pool[DS_POOL];
static int g_host_rate = 44100;

/* Schwung's standard knob grid, one page, the eight knobs in order. Served
 * as ui_hierarchy, so the shadow UI opens it in a track slot AND in Master
 * FX (which opens nothing for a module without one). */
static const char k_hierarchy[] =
    "{\"modes\":null,\"levels\":{"
      "\"root\":{\"label\":\"DEEP SPACE\","
        "\"knobs\":[\"space\",\"depth\",\"size\",\"decay\",\"diffusion\",\"darkness\",\"freeze\",\"mix\"],"
        "\"params\":[\"space\",\"depth\",\"size\",\"decay\",\"diffusion\",\"darkness\",\"freeze\",\"mix\"]}"
    "}}";

static const char *const *enum_names(int idx, int *count) {
    if (idx == DS_P_SPACE) {
        *count = DS_SPACE_COUNT;
        return DS_SPACE_NAMES;
    }
    if (idx == DS_P_FREEZE) {
        *count = 2;
        return DS_FREEZE_NAMES;
    }
    *count = 0;
    return NULL;
}

static int param_index(const char *key) {
    for (int i = 0; i < DS_P_COUNT; ++i)
        if (!strcmp(key, DS_PARAMS[i].key)) return i;
    return -1;
}

static const char *skip_space(const char *p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

/* A number, or (enums) an option name in any case. */
static int parse_value(int idx, const char *text, float *out) {
    int count;
    const char *const *names = enum_names(idx, &count);
    const char *p = skip_space(text);
    for (int i = 0; i < count; ++i) {
        size_t n = strlen(names[i]);
        if (!strncasecmp(p, names[i], n) && !*skip_space(p + n)) {
            *out = (float)i;
            return 1;
        }
    }
    char *end;
    float v = strtof(p, &end);
    if (end == p || *skip_space(end) || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* Flat JSON object of numbers. Every saved parameter must be present (all
 * but FREEZE, which is never saved: a set reopening frozen would have an
 * empty reverb and no input to it); unknown keys are skipped. Nothing is
 * applied unless all is valid. */
static int restore_state(ds_engine *e, const char *text) {
    if (strlen(text) > 2048) return 0;
    float next[DS_P_COUNT];
    unsigned seen = 0;
    const unsigned need = ((1u << DS_P_COUNT) - 1) & ~(1u << DS_P_FREEZE);
    const char *p = skip_space(text);
    if (*p++ != '{') return 0;
    for (int field = 0; field < 64; ++field) {
        p = skip_space(p);
        if (*p++ != '"') return 0;
        char key[32];
        int n = 0;
        while (*p && *p != '"' && n < (int)sizeof(key) - 1) key[n++] = *p++;
        key[n] = 0;
        if (*p++ != '"') return 0;
        p = skip_space(p);
        if (*p++ != ':') return 0;
        p = skip_space(p);
        char *end;
        float v = strtof(p, &end);
        if (end == p || !isfinite(v)) return 0;
        p = end;
        int idx = param_index(key);
        if (idx >= 0 && idx != DS_P_FREEZE) {
            if (seen & (1u << idx)) return 0;
            seen |= 1u << idx;
            next[idx] = v;
        }
        p = skip_space(p);
        if (*p == '}') {
            if (*skip_space(p + 1)) return 0;
            if ((seen & need) != need) return 0;
            for (int i = 0; i < DS_P_COUNT; ++i)
                if (i != DS_P_FREEZE) ds_engine_set(e, i, next[i]);
            ds_engine_set(e, DS_P_FREEZE, 0.0f);
            ds_engine_snap(e);
            return 1;
        }
        if (*p++ != ',') return 0;
    }
    return 0;
}

static int copy_text(char *buf, int size, const char *text) {
    size_t n = strlen(text);
    if (n >= (size_t)size) {
        buf[0] = 0;
        return -1;
    }
    memcpy(buf, text, n + 1);
    return (int)n;
}

static void *create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;
    ds_fft_global_init();
    for (int i = 0; i < DS_POOL; ++i) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_pool[i].used, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            ds_engine_init(&g_pool[i].engine, (float)g_host_rate, !g_pool[i].dirty);
            g_pool[i].dirty = 1;
            return &g_pool[i].engine;
        }
    }
    return NULL;
}

static void destroy_instance(void *instance) {
    for (int i = 0; i < DS_POOL; ++i) {
        if (instance == &g_pool[i].engine) {
            __atomic_store_n(&g_pool[i].used, 0, __ATOMIC_RELEASE);
            return;
        }
    }
}

static void process_block(void *instance, int16_t *audio, int frames) {
    if (!instance || !audio || frames <= 0) return;
    ds_engine_process((ds_engine *)instance, audio, frames);
}

static void set_param(void *instance, const char *key, const char *val) {
    if (!instance || !key || !val) return;
    ds_engine *e = (ds_engine *)instance;
    if (!strcmp(key, "state")) {
        (void)restore_state(e, val);
        return;
    }
    int idx = param_index(key);
    float v;
    if (idx < 0 || !parse_value(idx, val, &v)) return;
    ds_engine_set(e, idx, v);
}

static int format_value(const ds_engine *e, int idx, char *buf, int size) {
    int count;
    const char *const *names = enum_names(idx, &count);
    if (names) return snprintf(buf, size, "%s", names[(int)e->p[idx]]);
    return snprintf(buf, size, "%.4f", e->p[idx]);
}

static int get_param(void *instance, const char *key, char *buf, int size) {
    if (!instance || !key || !buf || size <= 0) return -1;
    buf[0] = 0;
    const ds_engine *e = (const ds_engine *)instance;
    int n = -1;
    int idx = param_index(key);
    if (idx >= 0) {
        n = format_value(e, idx, buf, size);
    } else if (!strcmp(key, "name")) {
        return copy_text(buf, size, "DEEP SPACE");
    } else if (!strcmp(key, "chain_params")) {
        return copy_text(buf, size, kChainParams);
    } else if (!strcmp(key, "ui_hierarchy")) {
        return copy_text(buf, size, k_hierarchy);
    } else if (!strcmp(key, "state")) {
        n = snprintf(buf, size,
            "{\"version\":1,\"space\":%d,\"depth\":%.6g,\"size\":%.6g,\"decay\":%.6g,"
            "\"diffusion\":%.6g,\"darkness\":%.6g,\"mix\":%.6g}",
            (int)e->p[DS_P_SPACE], e->p[DS_P_DEPTH], e->p[DS_P_SIZE], e->p[DS_P_DECAY],
            e->p[DS_P_DIFFUSION], e->p[DS_P_DARKNESS], e->p[DS_P_MIX]);
    } else if (!strcmp(key, "version")) {
        return copy_text(buf, size, kVersion);
    } else if (!strcmp(key, "nan_resets")) {
        n = snprintf(buf, size, "%d", e->nan_resets);
    } else if (!strcmp(key, "ir_builds")) {
        n = snprintf(buf, size, "%d", e->ir_builds);
    } else if (!strcmp(key, "fdn_clamps")) {
        n = snprintf(buf, size, "%d", e->fdn.clamps);
    }
    if (n < 0 || n >= size) {
        buf[0] = 0;
        return -1;
    }
    return n;
}

static audio_fx_api_v2_t g_api = {
    AUDIO_FX_API_VERSION_2,
    create_instance,
    destroy_instance,
    process_block,
    set_param,
    get_param,
    NULL,   /* on_midi: not used */
};

__attribute__((visibility("default")))
audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    if (host && host->sample_rate > 0) {
        if (host->sample_rate != 44100 && host->sample_rate != 48000) return NULL;
        g_host_rate = host->sample_rate;
    }
    ds_fft_global_init();
    return &g_api;
}
