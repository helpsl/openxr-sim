// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// COMPOSITING A FRAME'S LAYERS INTO AN EYE, in the simulator (src/composite.c). C, for the simulator's C++.
#ifndef OPENXR_SIM_COMPOSITE_H
#define OPENXR_SIM_COMPOSITE_H

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

// The canvas is B G R A (ARGB32 in memory), rows top to bottom, sRGB-encoded, opaque: what the window
// draws. `camera` gives the canvas's size.
void composite_clear(uint8_t *canvas, const struct oxs_camera *camera);
void composite_layer(uint8_t *canvas, const struct oxs_camera *camera, const struct oxs_layer *layer);

#ifdef __cplusplus
}
#endif

#endif
