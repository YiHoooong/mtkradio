// SPDX-License-Identifier: GPL-2.0-only
/*
 * Linux vfio-pci bring-up harness for MtkDevice (mtk_dev.cpp).
 *
 * Usage (with 0000:04:00.0 bound to vfio-pci):
 *   sudo ./mtkradio_linux --probe
 *   sudo ./mtkradio_linux --boot-test [channel] [--firmware DIR] [--verbose]
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "mtk_dev.h"
#include "mtk_platform.h"
#include "radio_link.h"

namespace {

volatile sig_atomic_t g_stop = 0;
void on_sig(int) { g_stop = 1; }

void log_line(const std::string &line) {
  std::fprintf(stderr, "[mtk-vfio] %s\n", line.c_str());
  std::fflush(stderr);
}

bool is_awdl_bssid(const uint8_t *frame, size_t len) {
  if (len < 22) return false;
  static constexpr uint8_t kAwdl[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
  return std::memcmp(frame + 16, kAwdl, 6) == 0;
}

uint16_t channel_to_mhz(uint8_t ch) {
  if (ch >= 1 && ch <= 13) return static_cast<uint16_t>(2407 + ch * 5);
  if (ch == 14) return 2484;
  return static_cast<uint16_t>(5000 + ch * 5);
}

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

void build_rx_wire(const mtk::RxFrame &f, std::vector<uint8_t> &out) {
  MinimalRadiotap rt{};
  rt.version = 0;
  rt.length = sizeof(rt);
  rt.present = (1u << 3) | (1u << 5);
  rt.freq_mhz = channel_to_mhz(f.channel);
  rt.flags = (f.channel <= 14) ? 0x00a0u : 0x0140u;
  rt.dbm = f.dbm;

  const uint32_t payload_len = static_cast<uint32_t>(sizeof(rt) + f.len);
  const radio_link_header hdr{static_cast<uint16_t>(RADIO_LINK_RX), 0, payload_len};

  out.resize(sizeof(hdr) + payload_len);
  std::memcpy(out.data(), &hdr, sizeof(hdr));
  std::memcpy(out.data() + sizeof(hdr), &rt, sizeof(rt));
  std::memcpy(out.data() + sizeof(hdr) + sizeof(rt), f.data, f.len);
}

bool write_all(int fd, const void *buf, size_t len) {
  const auto *p = static_cast<const uint8_t *>(buf);
  while (len > 0) {
    ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
    if (n <= 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool read_all(int fd, void *buf, size_t len) {
  auto *p = static_cast<uint8_t *>(buf);
  while (len > 0) {
    ssize_t n = ::recv(fd, p, len, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool send_msg(int fd, radio_link_type type, const void *payload, size_t len) {
  radio_link_header hdr{static_cast<uint16_t>(type), 0, static_cast<uint32_t>(len)};
  if (!write_all(fd, &hdr, sizeof(hdr))) return false;
  if (len > 0 && !write_all(fd, payload, len)) return false;
  return true;
}

} // namespace

int main(int argc, char **argv) {
  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);

  bool probe_only = false;
  std::string daemon_sock;
  uint8_t channel = 149;
  unsigned width = 80;
  unsigned sniff_sec = 8;
  mtk::MtkDevice::Options opts;
  opts.firmware_dir = "/home/hq/Desktop/MTKAWDL/nekodrop/mtkradio/firmware";

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--probe") probe_only = true;
    else if (a == "--boot-test") probe_only = false;
    else if (a == "--daemon") daemon_sock = next();
    else if (a == "--width") width = static_cast<unsigned>(std::atoi(next().c_str()));
    else if (a == "--firmware") opts.firmware_dir = next();
    else if (a == "--seconds") sniff_sec = static_cast<unsigned>(std::atoi(next().c_str()));
    else if (a == "--verbose") opts.verbose = true;
    else if (a[0] != '-') channel = static_cast<uint8_t>(std::atoi(a.c_str()));
  }
  if (channel <= 14) width = 20;

  mtk::MtkDevice dev(log_line);
  std::string err;

  if (probe_only) {
    if (!dev.probe(err)) {
      log_line("PROBE FAILED: " + err);
      return 1;
    }
    log_line("PROBE OK!");
    return 0;
  }

  if (!dev.open(opts, err)) {
    log_line("OPEN FAILED: " + err);
    return 1;
  }

  const uint8_t mac[6] = {0x02, 0x79, 0x22, 0xaa, 0xbb, 0xcc};
  if (!dev.start_monitor(channel, width, mac)) {
    log_line("start_monitor failed on ch " + std::to_string(channel));
    return 1;
  }

  if (!daemon_sock.empty()) {
    ::unlink(daemon_sock.c_str());
    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
      log_line("socket(AF_UNIX) failed");
      dev.close();
      return 1;
    }
    sockaddr_un sun{};
    sun.sun_family = AF_UNIX;
    std::snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", daemon_sock.c_str());
    if (::bind(srv, reinterpret_cast<sockaddr *>(&sun), sizeof(sun)) < 0 || ::listen(srv, 2) < 0) {
      log_line("bind/listen failed on " + daemon_sock);
      ::close(srv);
      dev.close();
      return 1;
    }
    log_line("listening on unix socket " + daemon_sock + " (ch " + std::to_string(channel) + ")");

    int client = -1;
    uint8_t bssid_filter[6] = {0};
    bool filter_active = false;
    std::vector<uint8_t> rx_wire;
    rx_wire.reserve(4160);
    std::vector<uint8_t> cmd_buf;
    cmd_buf.reserve(4160);
    uint64_t last_active_ms = 0;

    while (!g_stop) {
      size_t tx_drained = 0;
      if (client < 0) {
        pollfd pfd{srv, POLLIN, 0};
        if (::poll(&pfd, 1, 5) > 0 && (pfd.revents & POLLIN)) {
          client = ::accept(srv, nullptr, nullptr);
          if (client >= 0) {
            const int sock_buf = 4 << 20; /* 4 MiB socket buffers for AirDrop bursts */
            (void)::setsockopt(client, SOL_SOCKET, SO_RCVBUF, &sock_buf, sizeof(sock_buf));
            (void)::setsockopt(client, SOL_SOCKET, SO_SNDBUF, &sock_buf, sizeof(sock_buf));
            log_line("owl client connected");
            radio_link_hello hello{RADIO_LINK_VERSION, dev.channel(),
                                   static_cast<uint8_t>(dev.width_mhz()), {0, 0}};
            if (!send_msg(client, RADIO_LINK_HELLO, &hello, sizeof(hello))) {
              ::close(client);
              client = -1;
            }
          }
        }
      } else {
        pollfd pfd{client, POLLIN, 0};
        while (tx_drained < 64 && ::poll(&pfd, 1, 0) > 0 &&
               (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
          radio_link_header hdr{};
          if (!read_all(client, &hdr, sizeof(hdr)) || hdr.length > RADIO_LINK_MAX_PAYLOAD) {
            log_line("owl client disconnected");
            ::close(client);
            client = -1;
            break;
          }
          cmd_buf.resize(hdr.length);
          if (hdr.length > 0 && !read_all(client, cmd_buf.data(), hdr.length)) {
            log_line("owl client disconnected");
            ::close(client);
            client = -1;
            break;
          }
          tx_drained++;
          if (hdr.type == RADIO_LINK_TX) {
            const uint8_t *frame = nullptr;
            size_t frame_len = 0;
            mtk::TxParams tx_params{};
            if (mtk::parse_tx_radiotap(cmd_buf.data(), cmd_buf.size(), frame, frame_len,
                                       tx_params)) {
              dev.transmit(frame, frame_len, tx_params);
            }
          } else if (hdr.type == RADIO_LINK_SET_CHANNEL && cmd_buf.size() >= 1) {
            const uint8_t ch = cmd_buf[0];
            const unsigned bw = (ch <= 14) ? 20 : 80;
            if (ch != dev.channel()) dev.set_channel(ch, bw);
          } else if (hdr.type == RADIO_LINK_SET_ADDRESS && cmd_buf.size() >= 6) {
            dev.set_address(cmd_buf.data());
          } else if (hdr.type == RADIO_LINK_SET_BSSID_FILTER && cmd_buf.size() >= 6) {
            std::memcpy(bssid_filter, cmd_buf.data(), 6);
            filter_active = false;
            for (int i = 0; i < 6; i++) {
              if (bssid_filter[i] != 0) filter_active = true;
            }
          }
        }
      }

      const size_t n = dev.poll([&](const mtk::RxFrame &f) {
        if (client < 0) return;
        if (filter_active) {
          if (f.len < 22 || std::memcmp(f.data + 16, bssid_filter, 6) != 0) return;
        }
        build_rx_wire(f, rx_wire);
        if (!write_all(client, rx_wire.data(), rx_wire.size())) {
          ::close(client);
          client = -1;
        }
      });
      if (n > 0 || tx_drained > 0) {
        last_active_ms = mtk::plat::now_ms();
      } else if (mtk::plat::now_ms() - last_active_ms >= 2) {
        mtk::plat::sleep_ms(1);
      }
    }

    if (client >= 0) ::close(client);
    ::close(srv);
    ::unlink(daemon_sock.c_str());
    dev.close();
    return 0;
  }

  log_line("sniffing ch " + std::to_string(channel) + " (" + std::to_string(width) +
           " MHz) for " + std::to_string(sniff_sec) + "s...");

  /* Also verify raw 802.11 TX ring injection with a test AWDL Vendor-Specific Action frame. */
  const uint8_t test_awdl_frame[36] = {
      0xd0, 0x00, 0x00, 0x00,                         /* Action (No-Ack) */
      0x00, 0x25, 0x00, 0xff, 0x94, 0x73,             /* DA: AWDL multicast */
      0x02, 0x79, 0x22, 0xaa, 0xbb, 0xcc,             /* SA: our local MAC */
      0x00, 0x25, 0x00, 0xff, 0x94, 0x73,             /* BSSID: AWDL BSSID */
      0x10, 0x00,                                     /* SeqCtl = 1 */
      0x7f, 0x00, 0x17, 0xf2, 0x08, 0x00, 0x00, 0x00, /* Apple AWDL OUI header */
      0x00, 0x00, 0x00, 0x00};
  const bool tx_ok = dev.transmit(test_awdl_frame, sizeof(test_awdl_frame), 6);
  if (opts.verbose) {
    log_line(std::string("test TX injection: ") + (tx_ok ? "queued OK" : "FAILED"));
  }
  uint64_t total = 0, awdl = 0;
  const uint64_t end_ms = mtk::plat::now_ms() + sniff_sec * 1000ULL;
  while (!g_stop && mtk::plat::now_ms() < end_ms) {
    const size_t n = dev.poll([&](const mtk::RxFrame &f) {
      total++;
      if (is_awdl_bssid(f.data, f.len)) {
        awdl++;
        if (awdl <= 5) {
          log_line("AWDL frame #" + std::to_string(awdl) + " len=" + std::to_string(f.len) +
                   " ch=" + std::to_string(f.channel) + " dbm=" + std::to_string(f.dbm));
        }
      }
    });
    if (n == 0) mtk::plat::sleep_ms(1);
  }
  log_line("done: total_80211=" + std::to_string(total) + " awdl_frames=" + std::to_string(awdl));
  dev.close();
  return 0;
}
