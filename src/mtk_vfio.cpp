// SPDX-License-Identifier: GPL-2.0-only
/*
 * Linux vfio-pci backend implementing nekodrop's RegProxy and mtk::DmaPool32
 * so mtk_dev.cpp can be brought up and verified directly on real MT7922 hardware
 * on Linux (with IOMMU-isolated 32-bit DMA, or 1:1 IOMMU identity + pagemap
 * under MTK_USE_SYSFS=1) before testing on Windows.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <linux/vfio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "proxy.h"
#include "mtk_dma.h"

namespace {

struct VfioState {
  int container_fd = -1;
  int group_fd = -1;
  int dev_fd = -1;
  volatile uint8_t *bar0 = nullptr;
  size_t bar0_len = 0;
  std::vector<RegOp> close_ops;
};

VfioState g_vfio;

std::string read_sysfs_line(const std::string &path) {
  std::ifstream f(path);
  std::string s;
  if (f) std::getline(f, s);
  return s;
}

uint16_t read_sysfs_hex16(const std::string &path) {
  std::string s = read_sysfs_line(path);
  if (s.empty()) return 0;
  return static_cast<uint16_t>(std::strtoul(s.c_str(), nullptr, 16));
}

int find_iommu_group(const std::string &bdf) {
  char link[256] = {};
  const std::string p = "/sys/bus/pci/devices/" + bdf + "/iommu_group";
  ssize_t n = readlink(p.c_str(), link, sizeof(link) - 1);
  if (n <= 0) return -1;
  link[n] = '\0';
  const char *slash = std::strrchr(link, '/');
  return slash ? std::atoi(slash + 1) : -1;
}

} // namespace

/* --------------------------------------------------------------------------
 * RegProxy implementation over Linux vfio-pci
 * -------------------------------------------------------------------------- */
RegProxy::~RegProxy() {
  if (g_vfio.bar0 && !g_vfio.close_ops.empty()) {
    for (const auto &op : g_vfio.close_ops) {
      if (op.offset + 4 > g_vfio.bar0_len) continue;
      auto *p = reinterpret_cast<volatile uint32_t *>(g_vfio.bar0 + op.offset);
      if (op.kind == 1) *p = op.value;
      else if (op.kind == 2) *p = (*p | op.value);
      else if (op.kind == 3) *p = (*p & ~op.value);
    }
    g_vfio.close_ops.clear();
  }
  if (g_vfio.bar0) {
    munmap(const_cast<uint8_t *>(g_vfio.bar0), g_vfio.bar0_len);
    g_vfio.bar0 = nullptr;
    g_vfio.bar0_len = 0;
  }
  if (g_vfio.dev_fd >= 0) { close(g_vfio.dev_fd); g_vfio.dev_fd = -1; }
  if (g_vfio.group_fd >= 0) { close(g_vfio.group_fd); g_vfio.group_fd = -1; }
  if (g_vfio.container_fd >= 0) { close(g_vfio.container_fd); g_vfio.container_fd = -1; }
  _handle = nullptr;
}

bool RegProxy::open(std::string &error) {
  const char *env_bdf = std::getenv("MTK_PCI_BDF");
  const std::string bdf = env_bdf ? env_bdf : "0000:04:00.0";

  _vendor = read_sysfs_hex16("/sys/bus/pci/devices/" + bdf + "/vendor");
  _device = read_sysfs_hex16("/sys/bus/pci/devices/" + bdf + "/device");
  _subsystem = read_sysfs_hex16("/sys/bus/pci/devices/" + bdf + "/subsystem_device");

  /* Direct sysfs resource0 fallback when BIOS enforces 1:1 IOMMU (with identity mapping). */
  if (std::getenv("MTK_USE_SYSFS")) {
    const std::string cfg_path = "/sys/bus/pci/devices/" + bdf + "/config";
    int cfg_fd = ::open(cfg_path.c_str(), O_RDWR);
    if (cfg_fd >= 0) {
      uint16_t cmd = 0;
      if (pread(cfg_fd, &cmd, sizeof(cmd), 0x04) == sizeof(cmd)) {
        cmd |= 0x0006u; /* PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER */
        if (pwrite(cfg_fd, &cmd, sizeof(cmd), 0x04) != sizeof(cmd)) {
          close(cfg_fd);
          error = "pwrite(" + cfg_path + ") failed: " + std::strerror(errno);
          return false;
        }
      }
      close(cfg_fd);
    }

    const std::string rpath = "/sys/bus/pci/devices/" + bdf + "/resource0";
    g_vfio.dev_fd = ::open(rpath.c_str(), O_RDWR | O_SYNC);
    if (g_vfio.dev_fd < 0) {
      error = "open(" + rpath + ") failed: " + std::strerror(errno);
      return false;
    }
    const size_t bar_sz = 0x100000; /* 1 MiB BAR0 */
    void *m = mmap(nullptr, bar_sz, PROT_READ | PROT_WRITE, MAP_SHARED, g_vfio.dev_fd, 0);
    if (m == MAP_FAILED) {
      error = std::string("mmap(resource0) failed: ") + std::strerror(errno);
      return false;
    }
    g_vfio.bar0 = static_cast<volatile uint8_t *>(m);
    g_vfio.bar0_len = bar_sz;
    _bar_len = static_cast<uint32_t>(bar_sz);
    _handle = reinterpret_cast<void *>(1);
    return true;
  }

  const int group = find_iommu_group(bdf);
  if (group < 0) {
    error = "cannot resolve IOMMU group for " + bdf;
    return false;
  }

  g_vfio.container_fd = ::open("/dev/vfio/vfio", O_RDWR);
  if (g_vfio.container_fd < 0) {
    error = std::string("open(/dev/vfio/vfio) failed: ") + std::strerror(errno);
    return false;
  }
  if (ioctl(g_vfio.container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
      !ioctl(g_vfio.container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
    error = "VFIO Type1 IOMMU not supported";
    return false;
  }

  const std::string gpath = "/dev/vfio/" + std::to_string(group);
  g_vfio.group_fd = ::open(gpath.c_str(), O_RDWR);
  if (g_vfio.group_fd < 0) {
    error = "open(" + gpath + ") failed: " + std::strerror(errno) +
            " (is " + bdf + " bound to vfio-pci?)";
    return false;
  }

  vfio_group_status gs{};
  gs.argsz = sizeof(gs);
  if (ioctl(g_vfio.group_fd, VFIO_GROUP_GET_STATUS, &gs) < 0 ||
      !(gs.flags & VFIO_GROUP_FLAGS_VIABLE)) {
    error = "VFIO group " + std::to_string(group) + " is not viable";
    return false;
  }

  if (ioctl(g_vfio.group_fd, VFIO_GROUP_SET_CONTAINER, &g_vfio.container_fd) < 0) {
    error = std::string("VFIO_GROUP_SET_CONTAINER failed: ") + std::strerror(errno);
    return false;
  }
  if (ioctl(g_vfio.container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
    error = std::string("VFIO_SET_IOMMU failed: ") + std::strerror(errno);
    return false;
  }

  g_vfio.dev_fd = ioctl(g_vfio.group_fd, VFIO_GROUP_GET_DEVICE_FD, bdf.c_str());
  if (g_vfio.dev_fd < 0) {
    error = std::string("VFIO_GROUP_GET_DEVICE_FD failed: ") + std::strerror(errno);
    return false;
  }

  /* Reset device cleanly and enable PCI Memory Space + Bus Mastering. */
  ioctl(g_vfio.dev_fd, VFIO_DEVICE_RESET);

  vfio_region_info cfg_info{};
  cfg_info.argsz = sizeof(cfg_info);
  cfg_info.index = VFIO_PCI_CONFIG_REGION_INDEX;
  if (ioctl(g_vfio.dev_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg_info) == 0) {
    uint16_t cmd = 0;
    if (pread(g_vfio.dev_fd, &cmd, sizeof(cmd), cfg_info.offset + 0x04) == sizeof(cmd)) {
      cmd |= 0x0006u; /* PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER */
      [[maybe_unused]] ssize_t rc =
          pwrite(g_vfio.dev_fd, &cmd, sizeof(cmd), cfg_info.offset + 0x04);
    }
  }

  vfio_region_info bar0_info{};
  bar0_info.argsz = sizeof(bar0_info);
  bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
  if (ioctl(g_vfio.dev_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info) < 0) {
    error = std::string("VFIO_DEVICE_GET_REGION_INFO(BAR0) failed: ") + std::strerror(errno);
    return false;
  }

  void *m = mmap(nullptr, bar0_info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                 g_vfio.dev_fd, bar0_info.offset);
  if (m == MAP_FAILED) {
    error = std::string("mmap(BAR0) failed: ") + std::strerror(errno);
    return false;
  }

  g_vfio.bar0 = static_cast<volatile uint8_t *>(m);
  g_vfio.bar0_len = bar0_info.size;
  _bar_len = static_cast<uint32_t>(bar0_info.size);
  _handle = reinterpret_cast<void *>(1);
  return true;
}

uint32_t RegProxy::read32(uint32_t off) {
  if (!g_vfio.bar0 || off + 4 > g_vfio.bar0_len) {
    _failed = true;
    return 0xffffffffu;
  }
  return *reinterpret_cast<volatile uint32_t *>(g_vfio.bar0 + off);
}

void RegProxy::write32(uint32_t off, uint32_t value) {
  if (!g_vfio.bar0 || off + 4 > g_vfio.bar0_len) {
    _failed = true;
    return;
  }
  *reinterpret_cast<volatile uint32_t *>(g_vfio.bar0 + off) = value;
}

void RegProxy::write8(uint32_t off, uint8_t value) {
  if (!g_vfio.bar0 || off >= g_vfio.bar0_len) {
    _failed = true;
    return;
  }
  *(g_vfio.bar0 + off) = value;
}

void RegProxy::set_bits(uint32_t off, uint32_t bits) { write32(off, read32(off) | bits); }
void RegProxy::clear_bits(uint32_t off, uint32_t bits) { write32(off, read32(off) & ~bits); }

bool RegProxy::batch(std::vector<RegOp> &ops) {
  for (auto &op : ops) {
    if (op.kind == 0) op.value = read32(op.offset);
    else if (op.kind == 1) write32(op.offset, op.value);
    else if (op.kind == 2) set_bits(op.offset, op.value);
    else if (op.kind == 3) clear_bits(op.offset, op.value);
  }
  return !_failed;
}

bool RegProxy::set_close_ops(const std::vector<RegOp> &ops) {
  g_vfio.close_ops = ops;
  return true;
}

/* --------------------------------------------------------------------------
 * mtk::DmaPool32 implementation over VFIO Type1 IOMMU or sysfs 1:1 pagemap
 * -------------------------------------------------------------------------- */
namespace mtk {

namespace {
constexpr uint32_t kVfioIovaBase = 0x10000000u; /* 256 MiB, well below 4 GiB */
size_t g_vfio_dma_bytes = 0;
size_t g_sysfs_dma_bytes = 0;

size_t available_ram_bytes() {
  std::ifstream f("/proc/meminfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("MemAvailable:", 0) == 0) {
      std::istringstream iss(line.substr(13));
      size_t kb = 0;
      if (iss >> kb) return kb * 1024ULL;
    }
  }
  struct sysinfo si{};
  if (sysinfo(&si) == 0) return static_cast<size_t>(si.freeram) * si.mem_unit;
  return 512ULL << 20;
}

struct LowPage {
  uintptr_t pfn;
  uint8_t *va;
};

struct BatchMap {
  void *ptr;
  size_t bytes;
};
} // namespace

DmaPool32::~DmaPool32() {
  if (_view && g_vfio_dma_bytes) {
    if (g_vfio.container_fd >= 0) {
      vfio_iommu_type1_dma_unmap unmap{};
      unmap.argsz = sizeof(unmap);
      unmap.iova = kVfioIovaBase;
      unmap.size = g_vfio_dma_bytes;
      ioctl(g_vfio.container_fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
    }
    munmap(_view, g_vfio_dma_bytes);
    _view = nullptr;
    g_vfio_dma_bytes = 0;
  }
  if (_view && g_sysfs_dma_bytes) {
    munlock(_view, g_sysfs_dma_bytes);
    munmap(_view, g_sysfs_dma_bytes);
    _view = nullptr;
    g_sysfs_dma_bytes = 0;
  }
}

bool DmaPool32::init(size_t want_pages, size_t min_pages, std::string &error) {
  if (g_vfio.container_fd < 0) {
    if (!std::getenv("MTK_USE_SYSFS")) {
      error = "VFIO container not open";
      return false;
    }

    int pagemap_fd = ::open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) {
      error = std::string("open(/proc/self/pagemap) failed: ") + std::strerror(errno);
      return false;
    }

    const size_t avail = available_ram_bytes();
    const size_t max_hold_pages = (avail * 3 / 4) / kPageBytes;
    constexpr size_t kBatchPages = 4096; /* 16 MiB per batch */
    const size_t batch_bytes = kBatchPages * kPageBytes;

    std::vector<BatchMap> batches;
    std::vector<LowPage> low_pages;
    std::vector<uint64_t> pme(kBatchPages);
    size_t high_pages = 0;

    auto release_batches = [&]() {
      for (const auto &b : batches) {
        munlock(b.ptr, b.bytes);
        munmap(b.ptr, b.bytes);
      }
      batches.clear();
    };

    while (low_pages.size() < want_pages && high_pages < max_hold_pages) {
      void *mem = mmap(nullptr, batch_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (mem == MAP_FAILED) break;

      volatile uint8_t *vp = static_cast<volatile uint8_t *>(mem);
      for (size_t i = 0; i < kBatchPages; i++) vp[i * kPageBytes] = 0;
      if (mlock(mem, batch_bytes) < 0) {
        munmap(mem, batch_bytes);
        break;
      }

      const off_t pm_off =
          static_cast<off_t>((reinterpret_cast<uintptr_t>(mem) / kPageBytes) * sizeof(uint64_t));
      const ssize_t pm_bytes = static_cast<ssize_t>(kBatchPages * sizeof(uint64_t));
      if (pread(pagemap_fd, pme.data(), pm_bytes, pm_off) != pm_bytes) {
        munlock(mem, batch_bytes);
        munmap(mem, batch_bytes);
        break;
      }

      batches.push_back({mem, batch_bytes});
      for (size_t i = 0; i < kBatchPages; i++) {
        const uint64_t entry = pme[i];
        if (!(entry & (1ULL << 63))) continue; /* not present */
        const uintptr_t pfn = static_cast<uintptr_t>(entry & ((1ULL << 55) - 1));
        if (pfn > 0 && pfn < kMaxPfn32) {
          low_pages.push_back({pfn, static_cast<uint8_t *>(mem) + i * kPageBytes});
        } else {
          high_pages++;
        }
      }
    }
    close(pagemap_fd);

    if (low_pages.size() < min_pages) {
      release_batches();
      error = "only " + std::to_string(low_pages.size()) +
              " lockable 4 KiB pages below 4 GiB (" + std::to_string(min_pages) + " needed)";
      return false;
    }

    std::sort(low_pages.begin(), low_pages.end(),
              [](const LowPage &a, const LowPage &b) { return a.pfn < b.pfn; });

    const size_t keep = low_pages.size();
    const size_t view_bytes = keep * kPageBytes;
    void *view = mmap(nullptr, view_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (view == MAP_FAILED) {
      release_batches();
      error = std::string("mmap(_view) failed: ") + std::strerror(errno);
      return false;
    }

    for (size_t i = 0; i < keep; i++) {
      void *dst = static_cast<uint8_t *>(view) + i * kPageBytes;
      void *res = mremap(low_pages[i].va, kPageBytes, kPageBytes, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
      if (res == MAP_FAILED) {
        munmap(view, view_bytes);
        release_batches();
        error = std::string("mremap DMA page failed: ") + std::strerror(errno);
        return false;
      }
    }

    release_batches();
    if (mlock(view, view_bytes) < 0) {
      munmap(view, view_bytes);
      error = std::string("mlock(_view) failed: ") + std::strerror(errno);
      return false;
    }

    _view = static_cast<uint8_t *>(view);
    g_sysfs_dma_bytes = view_bytes;
    _frames.resize(keep);
    for (size_t i = 0; i < keep; i++) _frames[i] = low_pages[i].pfn;
    _taken.assign(keep, false);
    _cursor = 0;
    _back = keep;
    _spare.clear();
    _part = SIZE_MAX;
    _part_used = 0;
    _used_pages = 0;
    return true;
  }

  const size_t bytes = want_pages * kPageBytes;
  void *mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    error = std::string("mmap DMA pool failed: ") + std::strerror(errno);
    return false;
  }
  std::memset(mem, 0, bytes);

  vfio_iommu_type1_dma_map map{};
  map.argsz = sizeof(map);
  map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
  map.vaddr = reinterpret_cast<uint64_t>(mem);
  map.iova = kVfioIovaBase;
  map.size = bytes;
  if (ioctl(g_vfio.container_fd, VFIO_IOMMU_MAP_DMA, &map) < 0) {
    error = std::string("VFIO_IOMMU_MAP_DMA failed: ") + std::strerror(errno);
    munmap(mem, bytes);
    return false;
  }

  _view = static_cast<uint8_t *>(mem);
  g_vfio_dma_bytes = bytes;
  _frames.resize(want_pages);
  _taken.assign(want_pages, false);
  for (size_t i = 0; i < want_pages; i++) {
    _frames[i] = (kVfioIovaBase / kPageBytes) + i;
  }
  _cursor = 0;
  _back = want_pages;
  return true;
}

DmaBuf DmaPool32::page_at(size_t index) {
  DmaBuf b;
  b.va = _view + index * kPageBytes;
  b.pa = static_cast<uint32_t>(_frames[index] * kPageBytes);
  b.size = kPageBytes;
  std::memset(b.va, 0, kPageBytes);
  return b;
}

DmaBuf DmaPool32::alloc_page() {
  if (!_spare.empty()) {
    const size_t i = _spare.back();
    _spare.pop_back();
    _taken[i] = true;
    _used_pages++;
    return page_at(i);
  }
  while (_back > _cursor) {
    const size_t i = --_back;
    if (_taken[i] || !_frames[i]) continue;
    _taken[i] = true;
    _used_pages++;
    return page_at(i);
  }
  return {};
}

DmaBuf DmaPool32::alloc(size_t size, size_t align) {
  if (!size || !_view) return {};
  if (align == 0) align = 1;
  if (size <= kPageBytes / 2 && align <= kPageBytes) {
    if (_part != SIZE_MAX) {
      const size_t off = (_part_used + align - 1) & ~(align - 1);
      if (off + size <= kPageBytes) {
        DmaBuf b;
        b.va = _view + _part * kPageBytes + off;
        b.pa = static_cast<uint32_t>(_frames[_part] * kPageBytes + off);
        b.size = size;
        std::memset(b.va, 0, size);
        _part_used = off + size;
        return b;
      }
    }
    DmaBuf p = alloc_page();
    if (!p) return {};
    _part = static_cast<size_t>(p.va - _view) / kPageBytes;
    _part_used = size;
    p.size = size;
    return p;
  }

  const size_t need = (size + kPageBytes - 1) / kPageBytes;
  if (need == 1) {
    DmaBuf p = alloc_page();
    if (p) p.size = size;
    return p;
  }

  while (_cursor + need <= _back) {
    size_t run = 1;
    while (run < need && _cursor + run < _back &&
           !_taken[_cursor + run] && _frames[_cursor + run] == _frames[_cursor + run - 1] + 1) {
      run++;
    }
    if (run == need && !_taken[_cursor]) {
      const size_t start = _cursor;
      for (size_t k = 0; k < need; k++) _taken[start + k] = true;
      _cursor += need;
      _used_pages += need;
      DmaBuf b;
      b.va = _view + start * kPageBytes;
      b.pa = static_cast<uint32_t>(_frames[start] * kPageBytes);
      b.size = size;
      std::memset(b.va, 0, need * kPageBytes);
      return b;
    }
    for (size_t k = 0; k < run; k++) {
      if (!_taken[_cursor + k]) _spare.push_back(_cursor + k);
    }
    _cursor += run;
  }
  return {};
}

void DmaPool32::trim() {}

} // namespace mtk
