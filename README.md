# mtkradio

Standalone user-mode **MediaTek Connac2 (`MT7922` / `MT7921`)** 802.11 raw-frame / Apple AWDL radio daemon for **Windows** and **Linux**.

`mtkradio` drives the MediaTek PCIe WFDMA rings, MCU firmware boot (ROM patch + RAM firmware), channel tuning (20/40/80 MHz), HW RX BlockAck (`STA_REC_BA`), HT/VHT 80 MHz 2x2 peer station records (`STA_REC_HT` / `STA_REC_VHT` / `WTBL`), and raw 802.11 frame TX/RX entirely from user space:

- **Windows**: Talks to the PCIe BAR0 registers through `winawdl_regproxy` (`IOCTL_REGPROXY_*`) and locks 32-bit DMA pages via `AllocateUserPhysicalPages` (`SeLockMemoryPrivilege`), exposing a named pipe (`\\.\pipe\mtkradio` / `radio_link.h`) compatible with [nekodrop](https://github.com/YiHoooong/nekodrop).
- **Linux**: Uses `vfio-pci` (`mtk_vfio.cpp`) for BAR0 MMIO + IOMMU DMA mapping and exposes a Unix-domain socket (`--socket <path>`) compatible with [owly](https://github.com/t4t5/owly) ([PR #138](https://github.com/t4t5/owly/pull/138)).

## Supported Hardware

| Chip | PCI Vendor:Device | Status |
|------|-------------------|--------|
| MediaTek MT7922 / AMD RZ616 | `14c3:0616`, `14c3:7922` | Verified on Linux (end-to-end AirDrop RX/TX with iPhone/iPad) and Windows 11 (firmware boot, 5 GHz 80 MHz RX & RF TX) |
| MediaTek MT7921 / MT7921K | `14c3:7961`, `14c3:0608` | Firmware & register profile included |

## Firmware Files

`mtkradio` loads the standard `linux-firmware` MediaTek blobs (not redistributed in this repository):

- **MT7922**: `WIFI_MT7922_patch_mcu_1_1_hdr.bin` and `WIFI_RAM_CODE_MT7922_1.bin`
- **MT7921**: `WIFI_MT7961_patch_mcu_1_2_hdr.bin` and `WIFI_RAM_CODE_MT7961_1.bin`

## Build

### Linux (`mtkradio_linux`)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Windows (`mtkradio.exe`)

Open a **Visual Studio x64 Native Tools Command Prompt** (or run `vcvars64.bat`) and run:

```powershell
.\build.ps1
```

Or with CMake directly:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Quick Usage

### Linux (with `vfio-pci`)

```bash
# Sniff ch 149 (80 MHz) for 10 seconds
sudo ./build/mtkradio_linux 0000:02:00.0 --fw-dir /lib/firmware/mediatek --ch 149 --bw 80

# Run as Unix-socket radio daemon for owly
sudo ./build/mtkradio_linux 0000:02:00.0 --fw-dir /lib/firmware/mediatek --socket /tmp/mtkradio.sock
```

### Windows (with `winawdl_regproxy`)

```powershell
# Boot firmware and sniff ch 149 (80 MHz) for 10 seconds (requires SYSTEM or SeLockMemoryPrivilege)
.\build\mtkradio.exe --fw-dir .\firmware --boot-test 149 --verbose

# Serve named pipe for nekodrop
.\build\mtkradio.exe --fw-dir .\firmware --pipe \\.\pipe\mtkradio
```

## Repository Layout

- `src/mtk_dev.h`, `src/mtk_dev.cpp` — Connac2 hardware state machine (MCU boot, WFDMA rings, channel/BW switch, peer STA/WTBL/BA management, TX/RX)
- `src/mtk_regs.h` — MT7922 / MT7921 register & descriptor definitions
- `src/mtk_dma.h`, `src/mtk_dma.cpp` — Windows 32-bit DMA page pool (`AllocateUserPhysicalPages`)
- `src/mtk_main.cpp` — Windows CLI & named-pipe daemon entry point
- `src/mtk_vfio.cpp`, `src/mtk_linux_test.cpp` — Linux `vfio-pci` backend & Unix-socket daemon
- `src/mtk_platform.h` — Platform abstraction (`now_us`, `sleep_us`, logging)
- `src/common/` — Shared IPC (`radio_link.h`, `pipe_server.h`) and Windows `regproxy` client (`proxy.h`, `proxy.cpp`, `regproxy_ioctl.h`, `plat.h`, `plat_win.cpp`)

## License

GPL-2.0-only (see [LICENSE](LICENSE)).
