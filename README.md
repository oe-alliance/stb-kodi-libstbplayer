# libstbplayer

`libstbplayer` is a small, versioned hardware-video backend API for Linux-based
Enigma2 set-top boxes. It lets a media application such as Kodi retain its
native demuxer, audio engine, clock, subtitles, OSD and player controls while
compressed video is decoded and displayed by the receiver's hardware.

The library was created for STB Kodi on OpenATV/OE-Alliance receivers. It is
not tied to Enigma2 at runtime: a frontend may stop Enigma2 before playback and
use the decoder directly, which makes the receiver's limited CPU and memory
available to the media application.

> **Project status:** active development. The public ABI is versioned, but the
> project is still in the `0.x` series. Hardware support depends on the exact
> receiver driver and vendor SDK, not only on the SoC name.

## What it does

`libstbplayer` provides:

- a stable C ABI between a media frontend and machine-specific video backends;
- runtime backend loading with ABI and backend-name validation;
- capability discovery for codecs, resolution and playback operations;
- compressed-packet submission with explicit backpressure;
- playback state, decoder PTS, flush, drain, pause and teardown operations;
- optional video-plane rectangle and visibility control;
- hardware backends for HiSilicon, Broadcom and Dreambox Amlogic receivers;
- a fake backend and loader test that require no STB hardware; and
- `stb-player-probe`, a small backend and capability diagnostic tool.

It deliberately does **not** provide a demuxer, an audio decoder, an audio
output, a subtitle renderer, a GUI, network mounts or a complete media player.
Those remain the responsibility of Kodi or another frontend.

| Responsibility | Owner |
| --- | --- |
| Input, demuxing and stream selection | Frontend |
| Audio decoding and output | Frontend |
| Master clock and A/V policy | Frontend |
| Subtitles, OSD and player UI | Frontend |
| Compressed video decoding | `libstbplayer` backend and STB driver |
| Video-plane presentation | Backend where supported |

This separation preserves normal Kodi behavior such as seeking, fast forward,
rewind, pause, audio-track switching and subtitles without using Enigma2 as an
external video player.

## Backends

Backend modules are installed below `${libdir}/stbplayer` and are selected by a
short validated name. A backend reports only the capabilities compiled for and
available on the target machine.

| Backend | Platform | Hardware interface | Notes |
| --- | --- | --- | --- |
| `hisi-dvb` | HiSilicon ARM receivers | HiSilicon AVPLAY/VO SDK, with Linux-DVB support where required | Vendor `libhi_*` libraries and `/dev/hi_vdec` plus `/dev/hi_vo` must be supplied by the receiver image. H.264 and HEVC input must be Annex B. |
| `bcm-dvb` | Broadcom ARM and MIPSel receivers | `/dev/dvb/adapter0/video0`, DVB video ioctls and PES framing | Supports driver-specific `normal`, `type2`, `dreambox` and `vuplus` framing variants. Optional codecs are selected at build time from the machine capabilities. |
| `dream-aml` | Dreambox One and Dreambox Two | Dreambox direct-frame DVB ABI and the Amlogic `amvideo` plane | Uses `/dev/dvb/adapter0/video0`, `/dev/amvideo_poll` and the Dreambox Amlogic sysfs controls. It is not the CoreELEC V4L2 path. H.264 and HEVC input must be Annex B. |
| `fake` | Development host | No hardware | Built only for tests; exercises loading, ABI negotiation, packet ownership, status, flush and teardown. |

### Codec support

The ABI defines MPEG-1/2, MPEG-4 Part 2, H.263, H.264/AVC, H.265/HEVC,
VC-1/WMV3, VP6/8/9, AV1, MJPEG, DivX/Xvid, Sorenson Spark, AVS and AVS2.
Defining a codec in the ABI does not mean that every backend or receiver can
decode it.

- HiSilicon currently advertises MPEG-1/2, MPEG-4 Part 2, H.263, H.264, HEVC,
  VC-1, VP8, VP9 and MJPEG.
- Broadcom always includes the common MPEG, DivX/Xvid, H.263 and H.264 paths.
  HEVC, VC-1/WMV3, VP6, VP8, VP9 and Spark are enabled per machine at build
  time.
- Dreambox Amlogic currently advertises MPEG-1/2, MPEG-4 Part 2,
  MSMPEG4v3, DivX/Xvid, H.263, H.264, HEVC, VC-1/WMV3, VP9, MJPEG, AVS and
  AVS2.

Always use `probe()` or `stb-player-probe` instead of assuming support from
this list. A vendor driver or SDK may have a known limitation even when the SoC
can decode a format. For example, VP9 is currently not usable in the vendor
Enigma2 stack on some HiSilicon MV200/MV300 receivers; `libstbplayer` cannot
work around a decoder failure that is also present in Enigma2.

### Hardware validated by the STB Kodi project

Representative runtime tests have covered:

- HiSilicon: Octagon SF8008, SX88 V2 and SX988; AX/Mutant HD61-class
  receivers; Zgemma H11; and Dinobot U53/U571 variants;
- Broadcom ARM: Mutant HD51, Dreambox DM900/DM920 and Vu+ Duo 4K SE/Zero 4K;
- Broadcom MIPSel: Dreambox DM820/DM7080, Vu+ Duo2/Solo2/Solo SE,
  Formuler F1, Mutant HD2400 and Edision OS Mega; and
- Dreambox Amlogic: Dreambox Two, with Dreambox One using the same playback
  platform.

This is a validation list, not an exhaustive compatibility guarantee. Receiver
families with different drivers may require a separate backend variant even
when they use the same chip. Octagon SF4008 and ET1x000-class receivers are not
currently enabled because of unresolved driver limitations.

## Requirements

For a development build:

- a C11 compiler;
- CMake 3.16 or newer;
- Linux headers, including the DVB video API, when building hardware backends;
- POSIX threads and the dynamic-loader library; and
- the receiver's kernel driver and vendor runtime libraries for hardware use.

No proprietary vendor libraries or firmware are distributed by this source
repository. An image maintainer must obtain and package them under terms that
permit their use and distribution.

## Building and testing

The default build creates the loader library, probe tool, fake backend and unit
test. It does not enable a hardware backend.

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTBP_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To install into a staging prefix:

```sh
cmake --install build --prefix /usr
```

Do not run the hardware backends on an unrelated desktop Linux system. They
expect set-top-box device nodes and driver behavior.

### HiSilicon

```sh
cmake -S . -B build-hisi \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTBP_BUILD_TESTS=OFF \
  -DSTBP_BUILD_HISI_DVB_BACKEND=ON
cmake --build build-hisi --parallel
```

Set `-DSTBP_HISI_MV310=ON` only for the MV310 SDK variant that requires
explicit AVPLAY decoder allocation. The HiSilicon build also installs
`stb-hisi-display`, a helper used by affected images to initialize the vendor
virtual screen before EGL starts.

Set `-DSTBP_HISI_LINUX_DVB=ON` for receiver drivers whose Enigma2
`dvbvideosink` feeds compressed video through `/dev/dvb/adapter0/video0`.
This mode uses the receiver's Linux-DVB stream types and PES framing instead
of opening a separate AVPLAY/VO window. It is a driver-family choice, not a
generic HiSilicon default; verified OE recipes currently enable it for the
GFutures HD60/HD61/HD66, AB-COM Pulse, Maxytec Multibox and Zgemma HiSilicon
families.

### Broadcom

```sh
cmake -S . -B build-bcm \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTBP_BUILD_TESTS=OFF \
  -DSTBP_BUILD_BCM_DVB_BACKEND=ON \
  -DSTBP_BCM_DVB_VARIANT=normal \
  -DSTBP_BCM_DVB_HAVE_HEVC=ON
cmake --build build-bcm --parallel
```

`STBP_BCM_DVB_VARIANT` accepts `normal`, `type2`, `dreambox` or `vuplus`.
The following Boolean options must match the receiver driver:

- `STBP_BCM_DVB_HAVE_HEVC`
- `STBP_BCM_DVB_HAVE_WMV`
- `STBP_BCM_DVB_HAVE_VP6`
- `STBP_BCM_DVB_HAVE_VP8`
- `STBP_BCM_DVB_HAVE_VP9`
- `STBP_BCM_DVB_HAVE_SPARK`
- `STBP_BCM_DVB_LIMITED_MPEG4V2`

Do not enable a codec merely because the CPU family is capable of decoding it.
The selected stream type and framing must also be implemented by the installed
DVB driver.

### Dreambox Amlogic

```sh
cmake -S . -B build-dream-aml \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTBP_BUILD_TESTS=OFF \
  -DSTBP_BUILD_DREAM_AML_BACKEND=ON
cmake --build build-dream-aml --parallel
```

This backend targets the Dreambox One/Two driver ABI. Other Amlogic devices
normally use a different kernel and video API and must not select it solely
because their SoC is made by Amlogic.

## Installed files

A normal installation contains:

```text
${libdir}/libstbplayer.so.0
${libdir}/pkgconfig/libstbplayer.pc
${includedir}/stbplayer/backend.h
${includedir}/stbplayer/loader.h
${bindir}/stb-player-probe
${libdir}/stbplayer/libstbplayer-backend-<name>.so
```

`stb-hisi-display` is additionally installed when the HiSilicon backend is
enabled.

## Runtime use

Users normally receive `libstbplayer` and exactly one machine-compatible
backend as part of an Enigma2 image. The library has no interactive player
application. `stb-player-probe` only verifies loading and reports capabilities;
it does not decode a test video.

```sh
stb-player-probe --backend hisi-dvb
stb-player-probe --backend bcm-dvb
stb-player-probe --backend dream-aml
```

A successful probe prints the backend version, codec and feature masks,
maximum dimensions, and maximum packet size. It requires read/write access to
the relevant decoder devices.

The STB Kodi integration selects a module with the environment variable:

```sh
export KODI_STBPLAYER_BACKEND=hisi-dvb
```

Use `bcm-dvb` or `dream-aml` on those platforms. This variable belongs to the
Kodi adapter; the generic loader API accepts the backend name directly.

## API overview

The public API is declared in `include/stbplayer/backend.h` and
`include/stbplayer/loader.h`. A frontend follows this lifecycle:

1. Load a validated module name with `stbp_backend_load()`.
2. Obtain `stbp_backend_api_v1` with `stbp_backend_api()`.
3. Call `create()` and then `probe()`.
4. Check the requested codec and operations against the returned capability
   masks.
5. Call `open()` with the stream format and clock configuration.
6. Submit complete compressed packets with `queue_packet()`.
7. Use status, buffer, pause, flush, drain or presentation operations only when
   advertised.
8. Call `close()`, `destroy()` and `stbp_backend_unload()` in that order.

### Packet and timestamp contract

- PTS, DTS and duration values use a 90 kHz time base.
- Use `STBP_PTS_NONE` when a timestamp is unavailable.
- `queue_packet()` returning `STBP_OK` means that the complete packet was
  consumed or copied. The caller may release its buffer.
- `STBP_AGAIN` means that no part of the packet was accepted. The caller must
  retry the same complete packet after backpressure has cleared.
- A backend must never silently accept a partial packet or cause the caller to
  submit already accepted bytes twice.
- `STBP_PACKET_DROP` is a presentation hint. Reference data must still reach
  the decoder unless decoded output can be suppressed without breaking decoder
  state.
- H.264 and HEVC backends currently require Annex-B codec data and packets.
  The frontend must convert length-prefixed AVC/HEVC input before `open()` and
  `queue_packet()`.
- Encrypted elementary streams are not accepted. Decryption must happen in an
  authorized frontend component before packets reach this API.

### ABI rules

The ABI boundary contains only C data types. C++ objects, STL types and vendor
headers must never cross it. Every extensible structure starts with
`struct_size`, and both sides negotiate `STBP_ABI_VERSION_1` when loading a
module.

Backend files have the fixed form
`libstbplayer-backend-<validated-name>.so`. Names may contain only the
characters accepted by the loader; arbitrary paths are intentionally rejected.
Every module exports only `stbp_backend_get_api()` as its public entry point.

When extending the API:

- append fields instead of reordering existing fields;
- use `struct_size` before reading optional trailing fields;
- bump the ABI version for an incompatible change;
- keep vendor constants and structures private to the backend; and
- advertise a feature only after its complete hardware contract has been
  verified on a real receiver.

## OpenEmbedded integration

Hardware backends are machine-specific even when receivers share the same CPU
tune. Their packages must therefore use a machine- or equivalent SoC-specific
package architecture. A generic package feed must not allow differently
configured backends to overwrite each other.

The OE-Alliance integration splits the modules into separate runtime packages:

```text
libstbplayer
libstbplayer-backend-hisi-dvb
libstbplayer-backend-bcm-dvb
libstbplayer-backend-dream-aml
```

The image recipe should select one backend from `MACHINE`, `SOC_FAMILY`, the
target architecture and verified driver capabilities. For example, use
`PACKAGE_ARCH = "${MACHINE}"` when compile-time backend settings differ by
receiver. The frontend package must depend on the matching backend rather than
installing all hardware modules indiscriminately.

## Troubleshooting

Start with the probe tool and verify the installed module and devices:

```sh
stb-player-probe --backend bcm-dvb
ls -l /usr/lib/stbplayer
ls -l /dev/dvb/adapter0/video0
```

For HiSilicon also check:

```sh
ls -l /dev/hi_vdec /dev/hi_vo
```

For Dreambox Amlogic also check:

```sh
ls -l /dev/amvideo_poll /sys/class/vfm/map
```

Common results:

- `no device`: the required module, device nodes, access rights or vendor
  runtime is missing;
- `ABI mismatch`: the loader and backend were built from incompatible API
  versions, or the requested name does not match the module;
- `again`: normal decoder backpressure; retry the unchanged packet later;
- `unsupported`: the codec, stream layout or requested operation was not
  advertised for this backend; and
- video works in Enigma2 but not through this library: compare the exact driver
  stream type, codec initialization, framing and clock behavior before adding
  model-specific workarounds.

Frontend logs include backend messages through `stbp_host_callbacks.log`.
Capture those messages together with the kernel log, receiver model, image
version, driver version, codec, resolution, frame rate and whether the same
sample works in Enigma2 when reporting a playback problem.

## Contributing

Contributions for additional open-source Enigma2 images and receiver platforms
are welcome. A change should include:

- the affected receiver, SoC, driver and image version;
- the exact codec and sample properties used for testing;
- a capability update only when real hardware testing confirms it;
- fake-backend or loader coverage for changes to the common ABI;
- clean teardown that restores the decoder and video plane for Enigma2; and
- no proprietary SDK binaries, confidential headers, credentials or test media
  without redistribution permission.

Please keep generic loader changes separate from vendor/backend changes where
possible. Do not solve a driver-family issue with a broad model assumption:
receivers using the same SoC can still expose incompatible driver interfaces.

## License and permitted use

`libstbplayer` is free and open-source software licensed under the
**GNU General Public License, version 2 or any later version**
(`GPL-2.0-or-later`). See [LICENSE](LICENSE) for the licensing notice and the
link to the complete license text.

The project is intended for use in open-source Enigma2 images. Such projects
may use, modify and redistribute it under the GPL, including the obligation to
provide the complete corresponding source for GPL-covered binaries and
derivative works.

Closed-source versions of `libstbplayer`, proprietary derivative integrations,
and redistribution that withholds source required by the GPL are not
permitted. The project does not offer a proprietary or closed-source licensing
exception. The GPL license text is authoritative; image maintainers are
responsible for ensuring that their complete method of integration and
distribution complies with it.

Copyright (C) 2026 OE-Alliance contributors.
