// SPDX-License-Identifier: GPL-2.0-only
/* What the hardware layer (iwx.cpp, iwx_sta.cpp, fw.cpp) needs from the
 * platform. The user-mode radio implements it in plat_win.cpp; the kernel
 * driver (kdriver/kplat.cpp) builds the same sources with NEKO_KERNEL. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifdef NEKO_KERNEL
#include "kplat.h"
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/* Monotonic microseconds. */
uint64_t now_us();
/* Waits at least `us` microseconds (sleeps from 2 ms up). */
void delay_us(unsigned us);
/* A full memory barrier before the card is told about new descriptors. */
void plat_fence();
/* Reads a whole file (firmware). */
bool plat_read_file(const std::string &path, std::vector<uint8_t> &out, std::string &error);
/* snprintf, everywhere the shared sources format text. */
int plat_snprintf(char *buf, size_t size, const char *fmt, ...);
