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
#include "awdl_policy.h"
#include "awdl_schedule.h"
#include "plat.h"
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
  bool no_block_ack = false;
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
    } else if (a == "--no-block-ack") {
      no_block_ack = true;
    } else if (a == "--country" || a == "--tap" || a == "--profile" || a == "--mac" || a == "--pipe") {
      (void)next_val(); /* accepted for compatibility with NekoDropService */
    } else if (!a.empty() && a[0] >= '0' && a[0] <= '9') {
      const int ch = std::atoi(a.c_str());
      if (ch > 0 && ch <= 233) channel = static_cast<uint8_t>(ch);
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

  std::array<uint8_t, 6> own_mac{};
  std::memcpy(own_mac.data(), default_mac, 6);
  uint16_t mgmt_seq = 0;

  /* Peer AWDL schedule & country/channel learning (reused from iwxradio). */
  awdl_sched::PeerSchedule sched{};
  sched.assumeZero = true;
  uint8_t sched_peer[6] = {};
  bool have_sched_peer = false;
  uint64_t sched_rx_us = 0;
  uint64_t gate_since_us = 0;
  uint64_t gate_limit_us = 0;
  uint8_t gate_kind = 0;
  uint8_t peer_seq[16] = {};
  unsigned peer5g = 0;
  awdl::PeerCountry peer_country;

  struct QueuedTx {
    std::vector<uint8_t> frame;
    mtk::TxParams params;
  };
  constexpr size_t kUnicastQueueMax = 128;
  std::deque<QueuedTx> unicast_q;

  std::vector<uint8_t> rx_wire;
  std::vector<std::array<uint8_t, 33>> addba_responses;
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
          const bool is_data = (frame[0] & 0x0cu) == 0x08u;
          const bool is_unicast = flen >= 10 && (frame[4] & 1u) == 0;
          const bool to_sched_peer = is_data && is_unicast && have_sched_peer &&
                                     std::memcmp(frame + 4, sched_peer, 6) == 0;
          if (to_sched_peer && sched.known()) {
            if (unicast_q.size() >= kUnicastQueueMax) unicast_q.pop_front();
            unicast_q.push_back({std::vector<uint8_t>(frame, frame + flen), tx_params});
          } else {
            dev.transmit(frame, flen, tx_params);
          }
        }
      } else if (c.type == RADIO_LINK_SET_CHANNEL && c.payload.size() >= 1) {
        const uint8_t ch = c.payload[0];
        dev.set_channel(ch, ch <= 14 ? 20 : width);
        sched.reset();
        have_sched_peer = false;
        gate_since_us = 0;
      } else if (c.type == RADIO_LINK_SET_ADDRESS && c.payload.size() >= 6) {
        std::memcpy(own_mac.data(), c.payload.data(), 6);
        dev.set_address(own_mac.data());
      } else if (c.type == RADIO_LINK_SET_BSSID_FILTER && c.payload.size() >= 6) {
        std::memcpy(bssid_filter.data(), c.payload.data(), 6);
        filter_active = false;
        for (uint8_t b : bssid_filter) {
          if (b != 0) filter_active = true;
        }
      }
    }

    addba_responses.clear();
    const size_t got = dev.poll([&](const mtk::RxFrame &f) {
      const uint64_t heard = now_us();
      if (f.len >= 16 && f.channel == dev.channel()) {
        const uint8_t *from = f.data + 10;
        const bool is_sched = have_sched_peer && std::memcmp(from, sched_peer, 6) == 0;
        const bool want = is_sched || !sched.fresh(heard);
        if (want && awdl::is_awdl_action(f.data, f.len)) {
          awdl_sched::PeerSyncInfo info{};
          bool bad = false;
          if (awdl_sched::peerSyncFromAction(f.data, f.len, &info, &bad)) {
            if (!is_sched) {
              sched.reset();
              std::memcpy(sched_peer, from, 6);
              have_sched_peer = true;
              gate_since_us = 0;
            }
            sched_rx_us = heard;
            sched.notePeerFrame(heard);
            sched.note(info, heard, dev.channel());
          } else if (is_sched) {
            if (bad) sched.invalidate();
            else {
              sched_rx_us = heard;
              sched.notePeerFrame(heard);
            }
          }
        } else if (is_sched) {
          sched_rx_us = heard;
          sched.notePeerFrame(heard);
        }
      }
      if (awdl::is_awdl_action(f.data, f.len)) {
        uint8_t seq[16];
        if (awdl::peer_sequence(f.data, f.len, seq) && std::memcmp(seq, peer_seq, 16) != 0) {
          std::memcpy(peer_seq, seq, 16);
          const unsigned ch = awdl::peer_channel(seq);
          if (ch && ch != peer5g) {
            peer5g = ch;
            log_line("event: peer-channel " + std::to_string(ch));
          }
        }
        char cc[3];
        if (awdl::peer_country(f.data, f.len, cc) && peer_country.note(cc)) {
          log_line("event: peer-country " + std::string(peer_country.confirmed));
        }
      }

      /* Auto-answer 802.11 ADDBA Request (awdld/OWL does not send ADDBA Response,
       * so without this MT7922 never activates HW RX BlockAck / A-MPDU). */
      if (!no_block_ack && f.len >= 33 && (f.data[0] & 0xfcu) == 0xd0u &&
          std::memcmp(f.data + 4, own_mac.data(), 6) == 0 &&
          f.data[24] == 3u && f.data[25] == 0u) {
        const uint8_t token = f.data[26];
        const uint16_t req_params = static_cast<uint16_t>(f.data[27] | (f.data[28] << 8));
        const bool immediate = (req_params & 2u) != 0;
        if (immediate && addba_responses.size() < 8) {
          /* awdld/OWL does not parse 802.11 A-MSDU frames; keep bit 0 (A-MSDU) = 0
           * just like iwxradio/awdl_policy.h so iPhone sends plain A-MPDU. */
          const uint16_t amsdu = 0u;
          const uint8_t tid = static_cast<uint8_t>((req_params >> 2) & 0x0fu);
          uint16_t win = static_cast<uint16_t>((req_params >> 6) & 0x03ffu);
          if (win == 0 || win > 64) win = 64;
          std::array<uint8_t, 33> resp{};
          resp[0] = 0xd0u; /* Action */
          std::memcpy(resp.data() + 4, f.data + 10, 6);  /* DA = peer */
          std::memcpy(resp.data() + 10, own_mac.data(), 6); /* SA = us */
          std::memcpy(resp.data() + 16, f.data + 16, 6); /* BSSID */
          const uint16_t sc = static_cast<uint16_t>((++mgmt_seq) << 4);
          resp[22] = static_cast<uint8_t>(sc & 0xffu);
          resp[23] = static_cast<uint8_t>(sc >> 8);
          resp[24] = 3u; /* Category: Block Ack */
          resp[25] = 1u; /* Action: ADDBA Response */
          resp[26] = token;
          /* 27..28 = Status 0 (Success) */
          const uint16_t rsp_params = static_cast<uint16_t>(amsdu | 2u | (static_cast<uint16_t>(tid) << 2) | (win << 6));
          resp[29] = static_cast<uint8_t>(rsp_params & 0xffu);
          resp[30] = static_cast<uint8_t>(rsp_params >> 8);
          /* 31..32 = Timeout 0 */
          addba_responses.push_back(resp);
        }
      }

      if (!pipe.connected()) return;
      if (filter_active && f.len >= 22 && std::memcmp(f.data + 16, bssid_filter.data(), 6) != 0)
        return;
      build_rx_radiotap(f, rx_wire);
      pipe.send(RADIO_LINK_RX, rx_wire);
    });
    for (const auto &resp : addba_responses) {
      /* transmit() triggers MtkDevice::snoop_tx_ba() -> mcu_register_peer + mcu_set_rx_ba. */
      dev.transmit(resp.data(), resp.size(), 6u);
    }

    /* Drain queued unicast Data frames when peer's AWDL window is open. */
    if (!unicast_q.empty()) {
      const uint64_t now = now_us();
      bool gate_closed = false;
      if (have_sched_peer && sched.known() && sched.fresh(now)) {
        const bool dozing = sched.presentEntries() <= awdl_sched::kPeerDozeMaxEntries &&
                            (!sched_rx_us || now - sched_rx_us > 100000);
        const auto gate = sched.sendGate(now, dozing, 4000);
        if (gate == awdl_sched::PeerSchedule::SendGate::Open) {
          gate_since_us = 0;
          gate_kind = 0;
        } else {
          const uint8_t kind = static_cast<uint8_t>(gate);
          if (!gate_since_us || kind != gate_kind) {
            gate_since_us = now;
            gate_kind = kind;
            gate_limit_us = gate == awdl_sched::PeerSchedule::SendGate::Away
                                ? sched.pauseLimitUS(now)
                                : sched.awUS() + 2000;
          }
          if (now - gate_since_us < gate_limit_us) gate_closed = true;
          else gate_limit_us = 0;
        }
      }
      if (!gate_closed) {
        while (!unicast_q.empty()) {
          const auto &q = unicast_q.front();
          dev.transmit(q.frame.data(), q.frame.size(), q.params);
          unicast_q.pop_front();
        }
      }
    }

    if (got == 0 && local.empty()) Sleep(1);
  }

  if (reader.joinable()) reader.join();
  dev.close();
  timeEndPeriod(1);
  return 0;
}
