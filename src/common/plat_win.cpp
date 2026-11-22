// SPDX-License-Identifier: GPL-2.0-only
/* plat.h for the user-mode radio. */
#include "plat.h"

#include <atomic>
#include <cstdio>

uint64_t now_us() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  /* Quotient and remainder: counter * 1000000 overflows after ~21 days of
   * uptime at 10 MHz (Codex review #13). */
  const uint64_t c = static_cast<uint64_t>(t.QuadPart), hz = static_cast<uint64_t>(freq.QuadPart);
  return c / hz * 1000000ull + c % hz * 1000000ull / hz;
}

void delay_us(unsigned us) {
  if (us >= 2000) {
    Sleep(us / 1000);
    return;
  }
  const uint64_t end = now_us() + us;
  while (now_us() < end) YieldProcessor();
}

void plat_fence() { std::atomic_thread_fence(std::memory_order_seq_cst); }

bool plat_read_file(const std::string &path, std::vector<uint8_t> &out, std::string &error) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize(size > 0 ? static_cast<size_t>(size) : 0);
  const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  if (got != out.size()) {
    error = "short read from " + path;
    return false;
  }
  return true;
}

#include <cstdarg>
int plat_snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}
