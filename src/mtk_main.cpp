// SPDX-License-Identifier: GPL-2.0-only
/*
 * mtkradio: user-mode MediaTek MT7922 / MT7921 radio daemon for nekodrop / winawdl.
 *
 * Modes:
 *   mtkradio --probe                     Safe test: open regproxy, read chip ID & driver-own
 *   mtkradio --boot-test [ch] [--width]  Boot firmware, tune, sniff 802.11/AWDL frames for 10s
 *   mtkradio <channel> [--width 20|40|80] [--firmware DIR] [--verbose]
 *                                        Serve \\.\pipe\winawdl-radio for awdld / nekodrop
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "mtk_dev.h"
#include "pipe_server.h"
#include "radio_link.h"

namespace {

std::atomic<bool> g_stop{false};
bool stopping() { return g_stop.load(); }

BOOL WINAPI on_console(DWORD) {
  g_stop = true;
  return TRUE;
}

void log_line(const std::string &line) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(stderr, "%02u:%02u:%02u.%03u mtkradio: %s\n", t.wHour, t.wMinute, t.wSecond,
               t.wMilliseconds, line.c_str());
  std::fflush(stderr);
}

std::string exe_dir() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string s = path;
  const size_t slash = s.find_last_of("\\/");
  return slash == std::string::npos ? "." : s.substr(0, slash);
}

uint16_t channel_to_mhz(uint8_t ch) {
  if (ch >= 1 && ch <= 13) return static_cast<uint16_t>(2407 + ch * 5);
  if (ch == 14) return 2484;
  return static_cast<uint16_t>(5000 + ch * 5);
}

/* Minimal 18-byte radiotap header carrying channel + dBm signal for awdld. */
#pragma pack(push, 1)
struct MinimalRadiotap {
  uint8_t version;
  uint8_t pad;
  uint16_t length;
  uint32_t present; /* bit 3 = Channel, bit 5 = Antenna signal (dBm) */
  uint16_t freq_mhz;
  uint16_t flags;
  int8_t dbm;
  uint8_t pad2;
};
#pragma pack(pop)

void build_rx_radiotap(const mtk::RxFrame &f, std::vector<uint8_t> &out) {
  MinimalRadiotap rt{};
  rt.version = 0;
  rt.length = sizeof(rt);
  rt.present = (1u << 3) | (1u << 5);
  rt.freq_mhz = channel_to_mhz(f.channel);
  rt.flags = (f.channel <= 14) ? 0x00a0u : 0x0140u; /* 2 GHz CCK/OFDM vs 5 GHz OFDM */
  rt.dbm = f.dbm;

  out.resize(sizeof(rt) + f.len);
  std::memcpy(out.data(), &rt, sizeof(rt));
  std::memcpy(out.data() + sizeof(rt), f.data, f.len);
}

bool is_awdl_bssid(const uint8_t *frame, size_t len) {
  if (len < 22) return false;
  static constexpr uint8_t kAwdl[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
  return std::memcmp(frame + 16, kAwdl, 6) == 0;
}

struct PendingCmd {
  radio_link_type type;
  std::vector<uint8_t> payload;
};

} // namespace

int main(int argc, char **argv) {
  SetConsoleCtrlHandler(on_console, TRUE);
  timeBeginPeriod(1);

  bool probe_only = false;
  bool reset_only = false;
  bool boot_test = false;
  uint8_t channel = 149;
  unsigned width = 80;
  mtk::MtkDevice::Options dev_opts;
  dev_opts.firmware_dir = exe_dir() + "\\firmware";

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const auto next_val = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--probe") {
      probe_only = true;
    } else if (a == "--reset") {
      reset_only = true;
    } else if (a == "--boot-test") {
      boot_test = true;
    } else if (a == "--width") {
      width = static_cast<unsigned>(std::atoi(next_val().c_str()));
    } else if (a == "--firmware") {
      dev_opts.firmware_dir = next_val();
    } else if (a == "--verbose") {
      dev_opts.verbose = true;
    } else if (a[0] != '-') {
      channel = static_cast<uint8_t>(std::atoi(a.c_str()));
    }
  }
  if (channel <= 14) width = 20;

  mtk::MtkDevice dev(log_line);
  std::string err;

  if (probe_only) {
    if (!dev.probe(err)) {
      log_line("PROBE FAILED: " + err);
      return 1;
    }
    log_line("PROBE OK! MTK chip ready for bring-up.");
    return 0;
  }

  if (reset_only) {
    if (!dev.hard_reset(err)) {
      log_line("RESET FAILED: " + err);
      return 1;
    }
    return 0;
  }

  if (!dev.open(dev_opts, err)) {
    log_line("OPEN FAILED: " + err);
    return 1;
  }

  /* Default locally-administered MAC until awdld sends RADIO_LINK_SET_ADDRESS. */
  const uint8_t default_mac[6] = {0x02, 0x79, 0x22, 0xaa, 0xbb, 0xcc};
  if (!dev.start_monitor(channel, width, default_mac)) {
    log_line("start_monitor failed on ch " + std::to_string(channel));
    return 1;
  }

  if (boot_test) {
    log_line("boot-test: sniffing ch " + std::to_string(channel) + " for 10 seconds...");
    const uint8_t test_awdl_frame[36] = {
        0xd0, 0x00, 0x00, 0x00,                         /* Action (No-Ack) */
        0x00, 0x25, 0x00, 0xff, 0x94, 0x73,             /* DA: AWDL multicast */
        0x02, 0x79, 0x22, 0xaa, 0xbb, 0xcc,             /* SA: our local MAC */
        0x00, 0x25, 0x00, 0xff, 0x94, 0x73,             /* BSSID: AWDL BSSID */
        0x10, 0x00,                                     /* SeqCtl = 1 */
        0x7f, 0x00, 0x17, 0xf2, 0x08, 0x00, 0x00, 0x00, /* Apple AWDL OUI header */
        0x00, 0x00, 0x00, 0x00};
    const bool tx_ok = dev.transmit(test_awdl_frame, sizeof(test_awdl_frame), 6);
    if (dev_opts.verbose) {
      log_line(std::string("test TX injection: ") + (tx_ok ? "queued OK" : "FAILED"));
    }
    uint64_t rx_total = 0, rx_awdl = 0;
    const uint64_t end_ms = GetTickCount64() + 10000;
    while (!stopping() && GetTickCount64() < end_ms) {
      const size_t n = dev.poll([&](const mtk::RxFrame &f) {
        rx_total++;
        if (is_awdl_bssid(f.data, f.len)) rx_awdl++;
      });
      if (n == 0) Sleep(1);
    }
    log_line("boot-test done: total_80211=" + std::to_string(rx_total) +
             " awdl_frames=" + std::to_string(rx_awdl));
    dev.close();
    return 0;
  }

  /* Full winawdl radio link daemon mode */
  PipeServer pipe(RADIO_LINK_PIPE_NAME, stopping);
  if (!pipe.valid()) {
    log_line("failed to create named pipe " RADIO_LINK_PIPE_NAME);
    return 1;
  }
  log_line("listening on " RADIO_LINK_PIPE_NAME " (ch " + std::to_string(channel) + ")");

  std::mutex cmd_mu;
  std::deque<PendingCmd> cmds;
  std::array<uint8_t, 6> bssid_filter{};
  bool filter_active = false;

  std::thread reader([&]() {
    while (!stopping()) {
      if (!pipe.wait_for_client()) continue;
      log_line("awdld client connected");

      radio_link_hello hello{RADIO_LINK_VERSION, dev.channel(),
                             static_cast<uint8_t>(dev.width_mhz()), {0, 0}};
      pipe.send(RADIO_LINK_HELLO,
                std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&hello), sizeof(hello)));

      while (!stopping() && pipe.connected()) {
        radio_link_header hdr{};
        if (!pipe.read_exact(&hdr, sizeof(hdr))) break;
        if (hdr.length > RADIO_LINK_MAX_PAYLOAD) break;
        std::vector<uint8_t> payload(hdr.length);
        if (hdr.length && !pipe.read_exact(payload.data(), hdr.length)) break;
        std::lock_guard<std::mutex> lk(cmd_mu);
        cmds.push_back({static_cast<radio_link_type>(hdr.type), std::move(payload)});
      }
      log_line("awdld client disconnected");
      pipe.drop_client();
    }
  });

  std::vector<uint8_t> rx_wire;
  while (!stopping()) {
    /* Process commands from awdld on the radio thread (MtkDevice is single-threaded). */
    std::deque<PendingCmd> local;
    {
      std::lock_guard<std::mutex> lk(cmd_mu);
      local.swap(cmds);
    }
    for (const auto &c : local) {
      if (c.type == RADIO_LINK_TX) {
        const uint8_t *frame = nullptr;
        size_t flen = 0;
        mtk::TxParams tx_params{};
        if (mtk::parse_tx_radiotap(c.payload.data(), c.payload.size(), frame, flen, tx_params)) {
          dev.transmit(frame, flen, tx_params);
        }
      } else if (c.type == RADIO_LINK_SET_CHANNEL && c.payload.size() >= 1) {
        const uint8_t ch = c.payload[0];
        dev.set_channel(ch, ch <= 14 ? 20 : width);
      } else if (c.type == RADIO_LINK_SET_ADDRESS && c.payload.size() >= 6) {
        dev.set_address(c.payload.data());
      } else if (c.type == RADIO_LINK_SET_BSSID_FILTER && c.payload.size() >= 6) {
        std::memcpy(bssid_filter.data(), c.payload.data(), 6);
        filter_active = false;
        for (uint8_t b : bssid_filter) {
          if (b != 0) filter_active = true;
        }
      }
    }

    const size_t got = dev.poll([&](const mtk::RxFrame &f) {
      if (!pipe.connected()) return;
      if (filter_active && f.len >= 22 && std::memcmp(f.data + 16, bssid_filter.data(), 6) != 0)
        return;
      build_rx_radiotap(f, rx_wire);
      pipe.send(RADIO_LINK_RX, rx_wire);
    });
    if (got == 0 && local.empty()) Sleep(1);
  }

  if (reader.joinable()) reader.join();
  dev.close();
  timeEndPeriod(1);
  return 0;
}
