/*
 * Deep Space Convolution - the parameter table shared by the engine, the
 * plugin and the tests.
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
 *
 * module.json's chain_params must agree with this table; scripts/metadata.py
 * checks keys, order, ranges and defaults at build time. SIZE is stored as a
 * factor and DECAY in seconds, so Schwung's standard knob grid prints them
 * itself; SPACE and FREEZE are enums (index), the rest 0..1.
 */
#ifndef DS_PARAMS_H
#define DS_PARAMS_H

enum {
    DS_P_SPACE,      /* IR preset, 0..5 */
    DS_P_DEPTH,      /* distance macro */
    DS_P_SIZE,       /* spatial scale, 0.5..4 */
    DS_P_DECAY,      /* late RT60, 1..30 s */
    DS_P_DIFFUSION,
    DS_P_DARKNESS,
    DS_P_FREEZE,     /* 0 off, 1 on */
    DS_P_MIX,
    DS_P_COUNT
};

typedef struct {
    const char *key;
    float min, max, def;
    int integer;
} ds_param_info;

static const ds_param_info DS_PARAMS[DS_P_COUNT] = {
    {"space",     0.0f, 5.0f,  0.0f, 1},
    {"depth",     0.0f, 1.0f,  0.6f, 0},
    {"size",      0.5f, 4.0f,  1.5f, 0},
    {"decay",     1.0f, 30.0f, 8.0f, 0},
    {"diffusion", 0.0f, 1.0f,  0.7f, 0},
    {"darkness",  0.0f, 1.0f,  0.4f, 0},
    {"freeze",    0.0f, 1.0f,  0.0f, 1},
    {"mix",       0.0f, 1.0f,  0.4f, 0},
};

#define DS_SPACE_COUNT 6
static const char *const DS_SPACE_NAMES[DS_SPACE_COUNT] = {
    "CATHEDRAL", "CAVERN", "TUNNEL", "HANGAR", "ABYSS", "VOID"};
static const char *const DS_FREEZE_NAMES[2] = {"OFF", "ON"};

#endif
