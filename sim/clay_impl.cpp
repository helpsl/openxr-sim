// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// CLAY'S ONE DEFINITION, COMPILED AS C++ like the code that uses it -- not as C beside stb in
// third_party_impl.c, where it was until 2026-11-05. Its enums are CLAY_PACKED_ENUM: `enum : uint8_t` in
// C++, and in C `enum __attribute__((__packed__))` with GCC and Clang -- one byte either way -- but with
// MSVC `__pragma(pack(push, 1)) enum`, which packs nothing (pack is for struct members) and leaves them
// four bytes. Every Clay struct then had one layout in the library and another in sim/ui.cpp: the first
// --screenshot under Wine read a float (760.0, the height) as an array's length, reported "no text
// measurement function" over and over, and grew to 10 GB queueing the errors.
#define CLAY_IMPLEMENTATION
#include "clay.h"
