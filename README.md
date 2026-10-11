# NX-Tether

USB tethering from a phone to a Nintendo Switch running Atmosphère. Plug the phone in, turn on
USB tethering, and the console sees a wired LAN connection — no Wi-Fi hotspot involved.

It consists of a system module (the bridge) and a Tesla/Ultrahand overlay (status and controls).

> **Status: experimental (0.1.5).** The bridge has been confirmed working on one setup: a
> Switch (Mariko, "v2") on firmware 22.5.0 with Atmosphère 1.12.0 and a Samsung Galaxy S26
> Ultra. Other consoles, firmware versions and phones are untested. Use it on emuMMC, and read
> the [AI disclaimer](#ai-disclaimer) at the bottom.

## Contents

- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Install](#install)
- [Usage](#usage)
- [The overlay](#the-overlay)
- [Configuration](#configuration)
- [Troubleshooting](#troubleshooting)
- [Uninstall and recovery](#uninstall-and-recovery)
- [Building](#building)
- [Source layout](#source-layout)
- [Credits and license](#credits-and-license)
- [AI disclaimer](#ai-disclaimer)

## How it works

The Switch only has drivers for a handful of USB LAN adapter chips. A tethered phone speaks
RNDIS or CDC-NCM instead, which the console does not understand. The system module

1. talks to the phone itself (RNDIS, CDC-NCM or CDC-ECM),
2. shows the console's LAN driver (the `eth` system module) a software **ASIX AX88772**
   adapter by intercepting that driver's `usb:hs` session, and
3. carries ethernet frames between the two.

To the rest of the system it looks as if a supported wired LAN adapter was plugged in.

Only `eth` is intercepted. Controllers, USB audio and every other USB client talk to the real
USB service untouched.

## Requirements

- A Switch running **Atmosphère 1.12.0** (the module is built against its libraries; a
  different Atmosphère version may need a rebuild against the matching Atmosphere-libs).
- Tested on firmware **22.5.0**. The design relies on the LAN driver living in the `eth`
  system module, which is the case from firmware 15.0.0 on.
- For the overlay: a Tesla-compatible overlay loader (Ultrahand or Tesla Menu with nx-ovlloader).
- A phone with USB tethering, and a way to connect it so that **the Switch is the USB host**
  (see [Usage](#usage)).
- About 1.2 MB of system memory.

## Install

1. Build the release zip (see [Building](#building)) or take one from the repository's
   releases if one is published.
2. Copy the contents of the zip to the root of the SD card, merging folders:

   ```
   atmosphere/contents/4200000000000E7B/exefs.nsp
   atmosphere/contents/4200000000000E7B/flags/boot2.flag
   atmosphere/contents/4200000000000E7B/mitm.lst
   atmosphere/contents/4200000000000E7B/toolbox.json
   switch/.overlays/usb-tether.ovl
   ```

3. Reboot the console. The module starts at boot; it cannot be started later, because it has
   to be in place before the LAN driver opens the USB service.

To update, overwrite the same files and reboot.

## Usage

1. Connect the phone so that the Switch is the USB host:
   - **Docked:** plug the phone into one of the dock's USB ports.
   - **Handheld:** a USB-C OTG adapter on the Switch plus the phone's normal cable. A plain
     C-to-C cable can also work, but the phone may decide to be the host; if "USB tethering"
     stays greyed out, set the phone's USB preferences to "USB controlled by: connected
     device".
2. On the phone, enable **USB tethering** (on Samsung: Settings → Connections → Mobile Hotspot
   and Tethering → USB tethering).
3. The console should now report a wired connection. If it does not pick it up by itself, run
   System Settings → Internet → Test Connection.

Notes:

- The Switch powers the phone while connected, so the console's battery drains faster in
  handheld mode.
- The phone has to re-enable tethering after it is unplugged; most phones turn it off on
  disconnect.
- Being on a wired connection through a phone does not change anything about how visible a
  modified console is to Nintendo's servers. Keep whatever precautions you already use.

## The overlay

Open *USB Tether* from Ultrahand / Tesla Menu.

| Item | Meaning |
|------|---------|
| Module | One-line summary from the system module. "not running" means the module is not loaded. |
| Link | State of the connection to the phone and the protocol in use (RNDIS, NCM, ECM). |
| Phone | USB vendor:product ID of the phone. |
| MAC | MAC address the phone assigned to the link. |
| Received / Sent | Traffic through the bridge since boot. |
| DHCP test | Address offered by the phone during the mode 1 link test. |
| Running mode | Mode the module booted in. |
| LAN driver hooked | Whether `eth` opened its USB session through the module. |
| Virtual adapter | `Not shown` → `Offered` → `Opened by driver` → `In use`. |
| Driver requests | Control requests `eth` has sent to the virtual adapter. |
| Buffer access | How the module reaches `eth`'s transfer buffers (`Mapped` or `Debug`). |
| Last error | Last result code, if any. |

Controls:

- **Bridge** – pause or resume the bridge without rebooting.
- **Mode after reboot** – cycles Bridge → Link test → Off and saves it to the config.
- **Reconnect phone** – drops and re-establishes the link to the phone.

## Configuration

`sdmc:/config/usb-tether/config.ini` is created on first boot.

| Key | Default | Meaning |
|-----|---------|---------|
| `mode` | `2` | `2` full bridge, `1` phone link test only (nothing in the system is intercepted; the module brings up the link and sends one DHCP request to prove data flows), `0` off. |
| `log_level` | `2` | `0` errors, `1` info, `2` debug, `3` per-packet trace (slow; only meant for short captures). |
| `dhcp_probe` | `1` | Send one DHCP request when the phone link comes up and the system is not using it, to prove that data flows. |
| `force_pid` | `0x0000` | USB product ID of the ASIX adapter to imitate, e.g. `0x7720`. `0` chooses automatically from what the system asks for. |
| `prefer_ncm` | `0` | When the phone offers both RNDIS and NCM, use NCM. |
| `rx_single_frame` | `0` | Hand the console one ethernet frame per USB transfer instead of packing several. Slower; for diagnosing crashes under heavy traffic. |

Changes take effect after a reboot. The generated file documents each key as well.

**Crash guard.** If the console goes down within 30 seconds of booting in bridge mode, or
within 30 seconds of the virtual adapter appearing, the next boot drops to mode 1 by itself
and writes that to the config. Re-enable mode 2 from the overlay when you want to retry.
Powering off within those 30 seconds triggers it too.

## Troubleshooting

Everything is logged to `sdmc:/config/usb-tether/log.txt` (rotated to `log.old.txt` at 1 MB).
It records:

- every USB interface on the bus (how the phone presents itself),
- the device filters `eth` registers (which adapters this firmware accepts),
- the RNDIS/NCM handshake with the phone,
- the first 400 control requests `eth` sends to the virtual adapter, and the first few data
  transfers in each direction,
- how the module reaches `eth`'s transfer buffers.

Common situations:

| Symptom | Things to check |
|---------|-----------------|
| Overlay says "not running" | The files are in `atmosphere/contents/4200000000000E7B/` and the console was rebooted. Overlay versions before 0.1.1 showed this even when the module was running. |
| Link stays "Waiting" | The phone is not the USB device, or tethering is off. Check the cable/adapter and the phone's USB preferences. |
| Link is "Up" but the adapter is "Not shown" | The module booted in mode 1 (possibly through the crash guard). Set "Mode after reboot" to Bridge and reboot. |
| "LAN driver hooked: No" in bridge mode | `mitm.lst` is missing or the module was started after boot. |
| Buffer access "FAILED" | The module could not reach `eth`'s memory on this firmware; the log has the details. |

If the console shows a fatal error (blue screen), note the error code and keep the newest
files in `atmosphere/crash_reports/` and `atmosphere/fatal_errors/`; they say which system
module went down and where.

When reporting a problem, include `log.txt`, the firmware and Atmosphère versions, and the
phone model.

## Uninstall and recovery

Delete `atmosphere/contents/4200000000000E7B` and `switch/.overlays/usb-tether.ovl` from the
SD card, and optionally `config/usb-tether`. Nothing else on the console is modified.

If the console will not boot with the module installed, remove the same folder from a PC or
with hekate's USB mass storage tool (Tools → USB tools → SD Card).

## Building

### Prerequisites

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `devkitA64`, `libnx`,
  `switch-tools` and `switch-libjpeg-turbo` (Atmosphere-libs includes its headers):

  ```
  dkp-pacman -S switch-dev switch-libjpeg-turbo
  ```

- `git`, `make`, `zip`.
- A libnx recent enough for Atmosphere-libs at the pinned commit: switchbrew/libnx
  `feebd026ca0f5dcc2119f46ad8e0d16ad3dd4973` or newer. If the packaged libnx is older, build
  and install libnx from source.
- The toolchain used for the tested build was devkitA64 with GCC 16.1.0. libstratosphere is
  compiled with `-Werror`, so a much older or newer compiler may need small fixes.

### Get the source

```
git clone --recursive https://github.com/dvnvdx/NX-Tether.git
cd NX-Tether
```

If the repository was cloned without `--recursive`:

```
git submodule update --init
```

The submodules are pinned:

| Path | Repository | Commit |
|------|------------|--------|
| `lib/Atmosphere-libs` | [Atmosphere-NX/Atmosphere-libs](https://github.com/Atmosphere-NX/Atmosphere-libs) | `aae2da0` (Atmosphère 1.12.0) |
| `lib/libtesla` | [WerWolv/libtesla](https://github.com/WerWolv/libtesla) | `f766e9b` |

### Build

```
export DEVKITPRO=/opt/devkitpro     # if not already set
make dist
```

| Target | Result |
|--------|--------|
| `make` | Builds the system module and the overlay. |
| `make sysmodule` | `sysmodule/out/nintendo_nx_arm64_armv8a/release/sysmodule.nsp` |
| `make overlay` | `overlay/usb-tether.ovl` |
| `make dist` | `nx-tether-<version>.zip`, laid out for the SD card root (also in `dist/`). |
| `make test` | Builds and runs the PC-side tests. |
| `make clean` | Removes all build output. |

The first build compiles libstratosphere and takes several minutes.

### Tests

The protocol code that does not depend on the console is header-only and tested on the PC. It
needs only a C++20 host compiler, not devkitPro:

```
make test
```

The tests drive the adapter model with the request sequences of the Linux and FreeBSD ASIX
drivers, and check the RNDIS/NCM framing against re-implementations of the Linux USB gadget
code that Android phones run.

### Naming

The project is called NX-Tether; the files on the console (`usb-tether.ovl`,
`config/usb-tether/`), the `usbtet` status service and the `ubt` source prefix keep the
original working name so that existing installs keep their configuration.

## Source layout

```
sysmodule/
  sysmodule.json           NPDM description (program ID, permissions)
  source/
    ubt_main.cpp           startup
    ubt_usbhs_mitm.*       usb:hs interception for eth, event relay, status service
    ubt_virtual_adapter.*  the virtual adapter's IPC objects, endpoints and transfer queues
    ubt_asix_model.hpp     AX88772 register model and bulk framing (portable, tested)
    ubt_client_memory.*    access to eth's transfer buffers
    ubt_phone_link.*       USB host side: discovery, RNDIS/NCM/ECM bring-up, RX/TX threads
    ubt_framing.hpp        RNDIS / NCM framing and the DHCP probe (portable, tested)
    ubt_guard.*            crash guard
    ubt_config.*, ubt_log.*
overlay/                   Tesla/Ultrahand overlay
tests/                     PC-side tests
lib/                       git submodules (Atmosphere-libs, libtesla)
```

## Credits and license

Licensed under the GNU General Public License, version 2 — see [LICENSE](LICENSE).

- [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) and
  [Atmosphere-libs](https://github.com/Atmosphere-NX/Atmosphere-libs) by SciresM and
  contributors (GPLv2).
- [libnx](https://github.com/switchbrew/libnx) by switchbrew.
- [libtesla](https://github.com/WerWolv/libtesla) by WerWolv (GPLv2).
- [MissionControl](https://github.com/ndeadly/MissionControl) by ndeadly (GPLv2), whose module
  skeleton and build files this project follows.
- Protocol behaviour was taken from the Linux and FreeBSD ASIX, RNDIS and CDC-NCM drivers and
  the [SwitchBrew wiki](https://switchbrew.org/).

This project is not affiliated with or endorsed by Nintendo, Samsung or ASIX.

## AI disclaimer

The code and documentation in this repository were written by an AI model (Claude, by
Anthropic) at the direction of the repository owner, who tested the result on their own
console. No human has reviewed the code line by line.

What that means in practice:

- Hardware testing is limited to the single setup named at the top of this file. The PC-side
  tests cover the protocol logic only, not the parts that talk to Horizon OS.
- The module runs with elevated privileges and intercepts a system service. A bug can crash
  the console or prevent it from booting until the module is removed from the SD card.
- Details about Horizon OS internals come from public documentation and open-source code, and
  some of them may be wrong for firmware versions other than the one tested.

Use it at your own risk, preferably on emuMMC, and review the source before relying on it.
The software comes with no warranty, as stated in the license.
