# libstbplayer

`libstbplayer` is a small, versioned hardware-media backend API for Linux-based
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
- compressed video and audio packet submission with explicit backpressure;
- playback state, decoder PTS, flush, drain, pause and teardown operations;
- optional video-plane rectangle and visibility control;
- hardware video backends for HiSilicon, Broadcom and Dreambox Amlogic receivers;
- a Linux-DVB hardware audio output with PES timestamps and driver A/V sync;
- `stb-player-probe`, a small backend and capability diagnostic tool.

It deliberately does **not** provide a demuxer, software audio decoder,
subtitle renderer, GUI, network mounts or a complete media player. Those remain
the responsibility of Kodi or another frontend.

| Responsibility | Owner |
| --- | --- |
| Input, demuxing and stream selection | Frontend |
| Software audio decoding and stream selection | Frontend |
| AC3/EAC3/DTS hardware decoding | DVB audio decoder where supported |
| STB hardware audio output | `libstbplayer` DVB audio API where supported |
| Master clock and A/V policy | Frontend plus the shared STB hardware clock |
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
| `bcm-dvb` | Broadcom ARM and MIPSel receivers | `/dev/dvb/adapter0/video0`, DVB video ioctls and PES framing | Supports driver-specific `normal`, `type2`, `dreambox`, `vuplus` and `gigablue` variants. Optional codecs are selected at build time from the machine capabilities. |
| `dream-aml` | Dreambox One and Dreambox Two | Dreambox direct-frame DVB ABI and the Amlogic `amvideo` plane | Uses `/dev/dvb/adapter0/video0`, `/dev/amvideo_poll` and the Dreambox Amlogic sysfs controls. It is not the CoreELEC V4L2 path. H.264 and HEVC input must be Annex B. |

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

The following list describes the established video backends. The new hardware
audio path has separate receiver test results documented below.

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

## Hardware audio output

The Linux-DVB audio path, enabled by default in OE builds, writes timestamped PES packets to
`/dev/dvb/adapter0/audio0`, enables `AUDIO_SET_AV_SYNC`, and reads the decoder
clock with `AUDIO_GET_PTS`. It is based on the same driver contract used by
Enigma2's `dvbaudiosink` and supports stereo S16 PCM plus hardware MPEG audio,
MP3, ADTS-framed AAC, AC3, EAC3 and DTS where the receiver driver implements those bypass
modes. AC3, EAC3 and DTS remain compressed, so their multichannel layout is
preserved for the receiver or AVR instead of being reduced to Kodi's stereo
ALSA output. Volume and mute use the same open decoder handle through
`AUDIO_SET_MIXER` and `AUDIO_SET_MUTE`, avoiding a second competing open of
the device.

Use `-DSTBP_DVB_AUDIO_VARIANT=dreambox` for the legacy Dreambox DVB bypass
numbering, or `dags` for normal bypass numbers with the DAGS `BCMA` PCM
framing. Other receiver families use `normal`. The frontend must pass each
audio buffer's original 90 kHz PTS to `stbp_audio_queue_packet()`; fabricating
timestamps from wall-clock time defeats hardware lipsync.

Dreambox BCM uses EAC3 bypass `7`, PCM bypass `15` with the `BCMA` private
header, and DTS bypass `2`. Its DTS payloads are normalized from 16-bit
little-endian or 14-bit packed data to 16-bit big-endian before PES submission,
matching the driver-facing format of the supplied DreamOS ARM sink. This is
an independent implementation; no proprietary DreamOS binary is shipped.
The DM900/DM920 build enables native DTS instead of the old GStreamer
software-downmix selection. DM820/DM7080 retain their E2 software
fallback. Existing driver proc
settings select downmix, multichannel PCM or passthrough where available.
Native DTS playback in E2 and the Kodi hardware-audio output have been
confirmed on DM920, including multichannel PCM output to an AVR. DM820/DM7080
share the Dream BCM integration but have not repeated the new Kodi audio
tests. Codec IDs and mocked I/O tests alone do not validate driver timing or
AVR output on another receiver.

The Kodi STB sink forwards AC3/EAC3 and DTS core without IEC61937
wrapping. Other formats use Kodi decoding followed by stereo PCM output.
GUI and keepalive audio use a continuous sample-based PES timeline; starting
the decoder with untimestamped PCM can stall it even when later packets have
valid timestamps. The sink clears GUI preroll when media timestamps first
arrive and limits queued audio using the decoder clock. Resume preroll is
measured from the first queued media PTS until the decoder clock starts.
Some drivers report elapsed PCM samples instead of media PTS. Enable
`STBP_DVB_AUDIO_PCM_SAMPLE_CLOCK` only after verifying that driver behavior;
currently it is measured on Duo 4K Lite. The capability flag lets the frontend
measure PCM queue delay from submitted samples and preserve a continuous GUI
timeline. Flushing this PCM path reopens the decoder because `AUDIO_CLEAR_BUFFER`
temporarily exposes the previous counter. Duo 4K Lite runtime tests confirmed
GUI/movie PCM audio, lipsync after seeking and audio after returning to Enigma2.
These PCM results do not validate other receiver drivers.
On Uno 4K SE, the PCM decoder uses media PTS, but may initially return a stale
timestamp from the previous stream. Kodi primes a bounded queue until the
timestamp belongs to the new timeline. Media-timed PCM uses hardware-clock
feedback without software sample dropping; sample-counter PCM keeps its
existing correction path. On 13 October 2026, Uno 4K SE tests confirmed
stutter-free DTS and AC3 playback in both downmix and passthrough modes with
Kodi test build `r117.unohw4` and `libstbplayer r68.unohw2`.
On SF8008 and its `libreader` driver family, native audio requires the
Linux-DVB video path as well: a separate video-only AVPLAY instance uses
`SYNC_REF_NONE` and cannot share the DVB audio decoder's clock. Keep
`libreader` running while Enigma2 is stopped; the vendor service owns both
decoders, not Enigma2. Its readiness must be checked before reading audio
proc controls. On 13 October 2026, SF8008 hardware tests confirmed DTS
lipsync after resume and seeking with this shared-driver path. The previous
test also confirmed audio after returning to Enigma2. Other members of this
driver family inherit the build setting but have not individually repeated
the new audio tests.
On the Pulse Mini's original HiSi DVB-driver family, `AUDIO_GET_PTS` can stay
unchanged after a seek although the decoder still plays audio. The optional
`STBP_DVB_AUDIO_HISI_VIDEO_CLOCK` path uses the active video decoder's real PTS
for media audio queue feedback, independent of codec. The backend registers its
existing descriptor and unregisters it before teardown; no extra decoder is
opened. Missing or unrelated
video timestamps fall back to the audio clock. Kodi marks film timestamps with
`STBP_AUDIO_PACKET_MEDIA_PTS`, including software-decoded film PCM; GUI audio,
sample-counter PCM and playback without hardware video keep their audio clock.
OE enables this only for the original HiSi Linux-DVB family, not SF8008/libreader
or Broadcom receivers.
Pulse Mini tests on 13 October 2026 confirmed synchronized DTS after repeated
seeks and stop/resume with the isolated clock test. AC3 reproduced the same
failure on the original package. The integrated Kodi `r121.pulsehw1` and
`libstbplayer r71.pulsehw1` then passed the user's AC3 and DTS seek and stop/resume
lipsync check; the log confirms media audio uses the video presentation clock.
Software-decoded film PCM has not passed regression testing with this
integrated package. A DivX5/MP3 AVI test failed with no picture or sound: the
HiSi backend rejected the DivX5 codec identifier, Kodi fell back to software
video, and PCM output repeatedly timed out without a hardware video clock.
The same AVI also produced no picture in Enigma2, but its audio worked there.
The user deferred this AVI case; it does not validate Kodi PCM with software
video. A generated H264/AAC sync clip also stalled at startup in Kodi: H264
hardware decoding opened, but the software-decoded PCM output repeatedly timed
out. The same clip produced picture and sound in Enigma2; this does not establish
that Enigma2 used PCM rather than its native AAC decoder. Standalone DVB PCM
tests also remained silent, including a test without hardware A/V waiting.
A separate native HiSi slave-track PCM test produced an audible tone, with
Enigma2 audio working after the test. This does not use Enigma2's whole-file
player: the vendor service keeps sound device 0 open, while Kodi owns only its
additional PCM track.

The optional `STBP_DVB_AUDIO_HISI_PCM_TRACK` integration uses that native track
for software-decoded stereo S16 PCM and leaves compressed AC3/DTS on the tested
DVB path. It resolves the SDK entry points already preloaded by the HiSi Kodi
starter; it neither opens/closes the shared sound device nor changes its
HDMI/SPDIF modes. Queue feedback uses submitted samples minus the SDK track
delay through the existing `PCM_SAMPLE_CLOCK` capability. This audio option
works independently of the Linux-DVB or AVPLAY video selection; it requires
a 32-bit HiSi build and the vendor sound device to remain initialized.
Pulse Mini hardware tests confirmed native PCM startup with the
H264/AAC clip: stereo S16 at 48 kHz, 480-frame packets and sample-clock feedback,
without the earlier startup audio timeouts. Seeking initially froze the first
target video frame while audio continued. A complete video stop/clear/play/
continue cycle fixed this in the isolated test; the user confirmed repeated
seeks, picture/sound synchronization, volume/mute and sound after returning
to Enigma2.

`libstbplayer r73.pulsehw1` integrates this recovery only while native HiSi PCM
is selected. Selection survives temporary track recreation during a concurrent
seek, but changes back when compressed audio opens. AC3/DTS retain their
existing flush sequence. Mocked tests cover PCM selection/teardown, both
compressed-codec exclusions, ioctl ordering and each failure point; normal
and Vu test configurations, cross-compilation and package QA passed. The
regular package is installed without diagnostic helpers. The user confirmed
the integrated AAC test, and its log verifies forward/backward seeks using
the native-PCM-only restart. AC3 and DTS regression tests also passed according
to the user; their logs confirm compressed hardware output, nonzero resume
and seeking. Short audio-output failures followed by successful sink reopening
still occur around compressed seeks, as previously observed before the PCM
change; these traces are not error-free. The native PCM restart is absent from
subsequent compressed seeks, although an initial resume can still occur while
the previous GUI PCM output is selected. On 14 October 2026, the user closed
this Pulse Mini test round and explicitly waived the additional saved AAC
resume test. That test remains unverified, not failed. Enigma2 audio recovery
was confirmed with the preceding test builds; the final r73 return-to-Enigma2
check was not separately reported. No further tests are scheduled for this round.

On 24 October 2026, SF8008 tests with `r74.sfpcm1` also confirmed improved
H.264/AAC MP4 startup and forward/backward seeks. The previous DVB PCM path
reset the shared libreader A/V instance when reopening audio, potentially
discarding already-submitted video. The native PCM track avoids those audio
resets. The user also confirmed AC3 and DTS playback with driver downmix;
these still use the compressed DVB decoder, not software PCM. A separate
Elysium 4K late-picture report is unresolved and was deferred by the user.

OE `r75` enables native PCM for the existing Linux-DVB families, the
SF8008/libreader family, DAGS Dual (`dagsmv200`) and Kodi-capable Dinobot
`u5*` receivers. The DAGS/Dinobot AVPLAY video paths and display-owner
processes are retained, as is each family's compressed-audio clock choice.
DAGS/Dinobot have not yet repeated the native PCM hardware tests; build
coverage must not be treated as confirmation of playback on those boxes.

TrueHD, DTS-HD and multichannel PCM are not advertised by this first version.
The driver's existing downmix/passthrough setting is preserved: a TV using
downmix still receives stereo; AC3/DTS 5.1 output requires the corresponding
driver passthrough setting and a compatible receiver.

The OE integration enables DVB hardware audio by default; no opt-in is needed.
It selects `main` and always applies the Kodi hardware-audio patches. The
`STB_HARDWARE_AUDIO` build switch and development-branch selection have been
removed. Commit and publish the matching library changes on `main` before
using the updated OE recipes; local receiver builds used the working tree
through externalsrc. `--audio-backend=alsa` explicitly selects the existing
ALSA output at runtime for comparison; it is not the default.

Validation proceeds from Duo 4K Lite and DM920 to GigaBlue Quad 4K, SF8008,
Pulse Mini and Dream Two. DM820/DM7080 share the Dream BCM integration; no
additional receiver test is scheduled for them. The remaining HiSilicon variants
and Dream Amlogic still require clock/decoder integration checks before the rollout
can be considered complete. Dream One/Two audio testing is deferred until
the new Dream Amlogic drivers are available. Default selection is not hardware
certification.
Broadcom hardware audio uses the driver's A/V clock rather
than Kodi's host-STC bridge, selected through `stbp_clock_config.kodi_is_clock_master`.

To verify the selected output, check Kodi's log for `STB Initialized` and
`CAESinkSTB::Initialize` with `opened STB hardware audio output`. Merely listing
`STB hardware audio` during device enumeration does not prove it is in use.
Decoder labels such as `ff-dca` describe software decoding, not the output
device: decoded PCM can still be submitted to the DVB hardware audio output.
Compressed playback must instead select a passthrough codec such as `pt-dts`
and open the STB sink with codec 5 (AC3), 6 (EAC3) or 7 (DTS). With the STB
sink selected, supported compressed formats are sent to the hardware decoder;
the receiver's driver modes decide downmix, passthrough or conversion. The
generic Kodi passthrough/receiver-capability switches are replaced by the
STB driver controls below. The saved `Default` device is resolved to the
enumerated STB device before format checks and packet packing.

Initial Duo 4K Lite AVR tests confirmed audible AC3 and DTS output after
disabling downmix in Enigma2. Full multichannel acceptance still requires:
verify its input format/channel indication, audible channels, lipsync,
seek/resume and audio after returning to Enigma2. The new settings UI and
other receiver families still require hardware validation.

### Receiver audio and video settings in Kodi

The development Kodi patch adds **STB audio output** under Settings > System >
Audio and **STB video output** under Settings > System > Display (Standard
settings level or higher). Controls are discovered from existing readable and
writable driver proc/sysfs nodes, not from receiver model names. Advertised
`*_choices` determine the available modes; fixed fallbacks are limited to known
Enigma2 interfaces. Missing or unusable controls are hidden.

The same driver modes are available in the player's Audio and Video settings
during playback. They update the same central values as the System menus and
use the same validation and video confirmation. With STB audio selected, these
modes replace the player's generic passthrough on/off toggle; the ALSA dialog
retains its existing toggle. The separate-preferences switches remain in the
System menus and also govern changes made during playback.

Both groups default to **Save separate Kodi settings: off**. Kodi reads the
current Enigma2/driver values at startup and permits session-only changes.
Enabling this option saves that group's Kodi preferences and reapplies supported
values on the next start. Disabling it returns the group to the original
driver values. On normal Kodi exit the original values are restored regardless
of this option; settings changed subsequently by another owner are not overwritten.
Kodi does not edit Enigma2's configuration file.

Audio controls include AC3, EAC3 and DTS modes, driver multichannel PCM routing,
HDMI audio source, surround and automatic-volume controls, where available.
On Dream drivers without a separate `dts` node, the DTS core mode uses the
`dtshd` node when it advertises `force_dts`. Kodi exposes it once as DTS and
restores the original value on exit; this does not enable DTS-HD transport.
Official Vu drivers share the AC3 output mode with DTS. Kodi labels this control
"Dolby Digital / DTS mode" and decodes DTS to stereo PCM in downmix mode, as
Enigma2 does. Passthrough retains the compressed DTS core for the AVR. Changes
also recheck the running DTS decoder; Gigablue/Lite and Dream hardware downmix
are unaffected.
AAC/AAC+, AAC transcoding, WMA Pro and DTS-HD mode values are displayed read-only
because this Kodi sink does not yet transport those formats natively. A driver
control does not add codec support: Kodi-decoded fallback PCM remains stereo.
Driver-decoded AC3/EAC3/DTS can retain multichannel audio when the driver and
connected receiver support the selected mode. The separate audio menu is hidden
when using the legacy ALSA backend.

Video controls include HDMI colour space, colourimetry, bit depth, HDR/HLG modes,
decoder sync mode and deinterlacing, where readable and supported. Each manual
video change requires confirmation within 15 seconds or is reverted. Existing
OSD resolution, refresh-rate selection and video-plane geometry remain unchanged.
Write-only command interfaces without a recoverable current value are not exposed.
Selecting a mode offered by a driver does not prove that the connected TV/AVR
supports it. Saved video preferences should therefore be disabled before changing
the HDMI equipment.

The audio device API is serialized by the caller. `STBP_OK` from packet submission
transfers ownership to the library; `STBP_AGAIN` means the new packet has not
been accepted and must be retried. A partial device write remains buffered.
PES headers and payloads are written separately, as in `dvbaudiosink`; some
vendor PCM parsers accept a combined write without actually decoding it.
Flush discards this pending data and the decoder queue. Drain is bounded and
returns `STBP_AGAIN` if the queued endpoint is not reached; a stationary decoder
PTS does not count as successful playback. Close returns the source to DVB demux.

## Building and testing

The default Linux build creates the loader library, DVB audio API and probe
tool. It does not enable a hardware video backend.
Use `-DSTBP_BUILD_DVB_AUDIO=OFF` when building the video-only library on another
host platform. Development-only tests and the fake backend are not bundled
in this source tree; the validation results above distinguish mocked checks
from tests on actual receivers.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
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
families. Native hardware-audio builds additionally enable it for SF8008,
SF8008 Mini, SX88 V2, SX988, Anadol IP8, Beyonwiz V2, Amiko Viper 4K,
Qviart OG2S/OG2OTT 4K, uClan Ustym 4K Pro/OTT Premium/S2 OTT X, and
GigaBlue IP 4K/Trio 4K/Trio 4K Pro (`MACHINE=gbmv200`). The Amiko Viper
4K here is `MACHINE=viper4k`, not the Dinobot-based Viper 4K V40 (`u571`).
DAGS `app_init` and Dinobot `showiframe` integrations are not switched by
this `libreader` family setting.

`-DSTBP_DVB_AUDIO_HISI_PCM_TRACK=ON` selects a separate native sound track
for decoded PCM on either video path. It does not require or enable
`STBP_HISI_LINUX_DVB`, and it does not select
`STBP_DVB_AUDIO_HISI_VIDEO_CLOCK`. Keep the latter restricted to drivers
whose compressed-audio PTS feedback requires that workaround.

### Broadcom

```sh
cmake -S . -B build-bcm \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTBP_BUILD_BCM_DVB_BACKEND=ON \
  -DSTBP_BCM_DVB_VARIANT=normal \
  -DSTBP_BCM_DVB_STARTUP_CATCHUP=ON \
  -DSTBP_BCM_DVB_HAVE_HEVC=ON
cmake --build build-bcm --parallel
```

`STBP_BCM_DVB_VARIANT` accepts `normal`, `type2`, `dreambox`, `vuplus` or
`gigablue`. The Gigablue variant preserves the vendor GStreamer startup order
and its separate PES-header/access-unit writes. It also does not clear the
decoder after `VIDEO_PLAY`; on BCM7252 a combined write prevents frames from
being presented, while the clear can restore the stale Enigma2 surface.

`STBP_BCM_DVB_NEXUS_STC` enables the optional Broadcom host-clock integration.
The backend first uses `/dev/stb-stc-host` when a compatible kernel bridge is
available and otherwise tries the userspace Nexus ABI. If neither interface is
usable, playback automatically retains decoder-PTS synchronization. Old Vu+
MIPSel drivers own the only full Nexus STC channel and expose no accessor for
the decoder's `SimpleStcChannel`; do not install the kernel bridge for these
receivers. Their decoder-PTS fallback has been verified with normal playback,
repeated seeks and stop/resume on a Vu+ Solo2.

`STBP_BCM_DVB_STARTUP_CATCHUP` enables startup resume-preroll handling for the
`normal` and `vuplus` Broadcom variants. Kodi's decode-only GOP is retained for
reference-frame reconstruction and consumed in temporary DVB trick mode until
the hardware PTS reaches the first displayable packet. This is required on the
normal receiver drivers and official Vu+ ARM drivers, where the occupied Nexus
STC channel is private and a long GOP would otherwise let audio advance before
video. A timeout always restores normal speed. The option stays disabled for
the already verified Vu+ MIPSel path and other Broadcom variants.

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
${includedir}/stbplayer/audio.h
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
- validation evidence for loader behavior and changes to the common ABI;
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
