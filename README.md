# plynic-mpv

[plynic](https://github.com/linrong123/plynic)'s fork of mpv. Baseline: the
upstream release tag `v0.41.0` (`41f6a64506`), plus upstream fixes
cherry-picked from master and a small patch stack, kept as commits on branch
`plynic/v0.41.0`. Branch `plynic/78d43740f5` (upstream `78d43740f5`, the
0.36-era commit media-kit's Android builds pinned) is frozen: it is what the
`v1.1.11-plynic.*` releases of plynic-libmpv-android were built from.

Built by [plynic-libmpv-android](https://github.com/linrong123/plynic-libmpv-android)
(`buildscripts/include/depinfo.sh` pins the commit) and
[plynic-libmpv-darwin](https://github.com/linrong123/plynic-libmpv-darwin)
(`flake.nix`/`flake.lock`, input `plynic-mpv`), both at the same commit, and
both tag their releases `v0.41.0-plynic.<n>`. Same license as upstream
mpv (LGPL-2.1+ with `-Dgpl=false`); the patches are offered under the same
terms. Upstream README follows below the line.

## Upstream fixes (cherry-picked, `git cherry-pick -x`)

One commit each, so a regression can be bisected to one of them:

- `66ff2efbc7` sub/sd_lavc: zero-initialize sub_bitmap entries (the OSD VO's
  sub-keepout reads those fields)
- `4bbf014f09` sub/draw_bmp: fix rgba alpha blending, and `9ccd8899ff`
  sub/draw_bmp: limit memset to available width in last slice
  (`mp_draw_sub_overlay()` is how `vo_mediacodec_osd` draws subtitles)
- `7ec8a7e9b1` vd_lavc: ensure decoder state is valid after reinit (the app
  switches `hwdec` at run time; without it hardware decoding could silently
  fall back to software after the reinit)
- `c1c047f8d6` stream/lavf: check child_next return value before dereference
- `fdb996adbc`, `fb5345809d`, `74271a7d80` sd_ass: animated-state reset and
  indexing, subtitle timing rounding
- `18d38d4f3e` demux: don't mark mid-stream cache ranges as BOF, and
  `4dc772c429` stream: don't drop data on stream_read_more (event HLS, first
  open)

Left out: `1388a45395` (ad_spdif muxer recreation; with passthrough),
`115b87b521`, `b48fb6c86c`, `dbe496e6be` (not needed on 0.41, or conflicting).

## plynic patches

Four topics, about 1.05k lines of code (the budget is five topics, ~1.5k
lines; see plynic's `docs/engine-strategy/README.md` §1):

1. **JavaVM hook** — `client: add mpv_lavc_set_java_vm() for Android embedders`
   (media_kit hands the JavaVM to libavcodec through it).
2. **Android OSD VO**
   - `vo: let a VO opt into OSD-only redraws without a frame`
     (`VO_CAP_OSD_ONLY_REDRAW`)
   - `vo_mediacodec_osd: add Android VO with a CPU-drawn OSD surface` — video
     via MediaCodec into one Surface, OSD/subtitles rendered by mpv into a
     second one (`vo-mediacodec-osd-surface`, `vo-mediacodec-osd-video-rect`)
   - `vo_mediacodec_osd: keep subtitles out of a bottom band the app covers`
     (`vo-mediacodec-osd-sub-keepout`), with the follow-ups `sub-keepout
     picks whole blocks and keeps a steady lift` and `sub-keepout only holds
     the lift across small differences`
   - `sub: make sub-keepout a subtitle option of every VO` — the same
     algorithm moved into `osd_render()` as `--sub-keepout`, so it also works
     through the render API (the texture path on iOS, macOS and phones);
     `vo-mediacodec-osd-sub-keepout` stays as an alias. A new lift bumps the
     subtitles' `change_id`, which is what every renderer's cache keys on
3. **Android audio and platform**
   - `ao_audiotrack: reload the AO when the AudioTrack dies` — a direct or
     offloaded track (multichannel PCM or passthrough over HDMI) is not
     restored by AudioTrack after a route change; it used to be recreated
     but never started (silence, video frozen until a seek)
   - `ao_audiotrack: don't spin while there is nothing to write` (underrun,
     EOF, or the core holding the buffer lock)
   - `ao_audiotrack: don't count the track buffer twice in the delay`
   - `ao_audiotrack: extrapolate timestamps in CLOCK_MONOTONIC`
   - `ao_audiotrack: re-sync timestamps after a route change` (speaker <->
     Bluetooth used to leave up to 3 s of A/V offset)
   - `ao_audiotrack: release everything when init() fails` (early returns
     kept the JNI use count up; on 0.41 this includes upstream's spdif check)
   - `ao_audiotrack: back off when reloads keep failing`
   - `ao_audiotrack: pause the track instead of resetting it` (a pause used
     to flush the 80-150 ms in the track)
   - `timer-linux: use CLOCK_MONOTONIC on Android` — Android's time base; on
     an arm64 3.18 kernel CLOCK_MONOTONIC_RAW jumps by ±453 s and mpv aborted
     as soon as a file was opened
4. **Darwin**
   - `meson: enable Objective-C on every Darwin host` — iOS cross builds and
     sandboxed builds (no xcrun) never detect a macOS SDK, which is where
     0.41 adds the language
   - `ao_audiounit: add --audiounit-skip-session-management` — leaves the
     shared AVAudioSession (category, mode, activation, deactivation,
     preferred channel count) to the app; same name and meaning as in
     media-kit's builds, without their reference counting or forced
     MixWithOthers
   - `stream_file: don't ask for the file system type on iOS` — fstatfs() is
     a privacy-manifest "required reason" API with no approved reason for
     this use, and iOS apps cannot mount network file systems anyway

   macOS builds with `-Dswift-build=enabled`: 0.41 guards some cocoa call
   sites with `HAVE_COCOA` only while their implementations need
   `HAVE_SWIFT` (`osdep/mac/app_bridge.m`, `player/main.c`), so a cocoa build
   without Swift would call into nothing. With Swift on, no guard commit is
   needed.

Each commit message says what changed against its `plynic/78d43740f5`
original. Dropped in the move: the backports of `46fe3cded0` (audiotrack JNI
multi-instance), `93a924a553` and `4d03efb4b0` (set_pause for pull AOs) —
upstream code since 0.38–0.40.

## Moving to a new upstream base: what bit last time

- `libmpv/client.h` is `include/mpv/client.h` since 0.40; git's rename
  detection carries the JavaVM hook over.
- `VO_CAP_*` bits: 0.41 took `1<<4..6` (`UNTIMED`, `FRAMEOWNER`, `VFLIP`);
  `VO_CAP_OSD_ONLY_REDRAW` is `1<<7`. `do_redraw()` returns for NORETAIN VOs
  under the lock after clearing `request_redraw` (`8798cec7fa`,
  `903c805a37`); the opt-out sits in that check. master rewrote the redraw
  path again, so expect a conflict there on 0.42.
- `draw_frame()` returns `bool` (`91c1b65de0`): `false` makes vo.c sleep
  through the frame and report the window as not visible.
- `ao_audiotrack.c` on 0.41: `pthread_*` → `mp_mutex`/`mp_cond`/`mp_thread`,
  the microsecond timer API is gone (`8f432b2e37`), `ao_read_data()` takes
  `eof`/`pad_silence`/`blocking` and only trylocks when non-blocking (0 can
  mean "core busy"), `timestamp_offset` was removed as dead (`87d30899ff`),
  and `init()` has an early spdif return (`3c1c848c2b`) that must take the
  error path.
- AudioTrack's content type (MOVIE/MUSIC) is only set with
  `--audio-set-media-role=yes` since `e99daecbdb`; the app sets it.
- The clock is chosen once, in `mp_raw_time_init()` (`891efca9d7`,
  `0a6c179026`), still preferring `CLOCK_MONOTONIC_RAW`.
- User-visible defaults: text subtitles are smaller (`8c3a7da619`: font
  55 → 38, border 3 → 1.65, margin 22 → 34); `sub-ass-override=yes` no longer
  applies `sub-scale` (only `scale`, the 0.41 default, does); `vo` and `wid`
  are `UPDATE_VO` (each change rebuilds the VO and seeks the whole player).
- `mpv-version` is stamped by the build as `v0.41.0-plynic-g<9 hex digits of
  the commit>`, identically on Android and Darwin, instead of `git describe`.
- `--sub-keepout` lives in `mp_osd_render_sub_opts` (change flag
  `UPDATE_OSD`, which is what redraws while paused) and is applied at the end
  of `osd_render()`; its state is in `osd_state` and scales with the height
  of whoever renders. If `osd_render()` or the renderers' `change_id` caching
  change upstream, re-check that a new lift still reaches every cache.
- Darwin: 0.41 only adds Objective-C once `TOOLS/macos-sdk-version.py`
  found a macOS SDK, and then also adds that SDK as link sysroot (wrong for
  iOS). With `b_lundef=false` (mpv's default) nothing complains about
  frameworks nobody links (AVFoundation, CoreVideo, OpenGLES, IOSurface,
  objc); plynic-libmpv-darwin links them and turns `b_lundef` on.
- Upstreaming (timer, the ao_audiotrack series, OSD-only redraw, the
  audiounit option, the objc meson fix): mpv's
  `DOCS/contribute.md` (`e76a35ec95`, master) does not accept commit messages
  or PR descriptions written by an AI; they have to be rewritten by hand
  first.

---

![mpv logo](https://raw.githubusercontent.com/mpv-player/mpv.io/master/source/images/mpv-logo-128.png)

# mpv


* [External links](#external-links)
* [Overview](#overview)
* [System requirements](#system-requirements)
* [Downloads](#downloads)
* [Changelog](#changelog)
* [Compilation](#compilation)
* [Release cycle](#release-cycle)
* [Bug reports](#bug-reports)
* [Contributing](#contributing)
* [License](#license)
* [Contact](#contact)


## External links


* [Wiki](https://github.com/mpv-player/mpv/wiki)
* [User Scripts](https://github.com/mpv-player/mpv/wiki/User-Scripts)
* [FAQ][FAQ]
* [Manual](https://mpv.io/manual/master/)


## Overview


**mpv** is a free (as in freedom) media player for the command line. It supports
a wide variety of media file formats, audio and video codecs, and subtitle types.

There is a [FAQ][FAQ].

Releases can be found on the [release list][releases].

## System requirements

- A not too ancient Linux (usually, only the latest releases of distributions
  are actively supported), Windows 10 1607 or later, or macOS 10.15 or later.
- A somewhat capable CPU. Hardware decoding might help if the CPU is too slow to
  decode video in realtime, but must be explicitly enabled with the `--hwdec`
  option.
- A not too crappy GPU. mpv's focus is not on power-efficient playback on
  embedded or integrated GPUs (for example, hardware decoding is not even
  enabled by default). Low power GPUs may cause issues like tearing, stutter,
  etc. On such GPUs, it's recommended to use `--profile=fast` for smooth playback.
  The main video output uses shaders for video rendering and scaling,
  rather than GPU fixed function hardware. On Windows, you might want to make
  sure the graphics drivers are current. In some cases, ancient fallback video
  output methods can help (such as `--vo=xv` on Linux), but this use is not
  recommended or supported.

mpv does not go out of its way to break on older hardware or old, unsupported
operating systems, but development is not done with them in mind. Keeping
compatibility with such setups is not guaranteed. If things work, consider it
a happy accident.

## Downloads


For semi-official builds and third-party packages please see
[mpv.io/installation](https://mpv.io/installation/).

## Changelog


There is no complete changelog; however, changes to the player core interface
are listed in the [interface changelog][interface-changes].

Changes to the C API are documented in the [client API changelog][api-changes].

The [release list][releases] has a summary of most of the important changes
on every release.

Changes to the default key bindings are indicated in
[restore-old-bindings.conf][restore-old-bindings].

Changes to the default OSC bindings are indicated in
[restore-osc-bindings.conf][restore-osc-bindings].

## Compilation


Compiling with full features requires development files for several
external libraries. Mpv requires [meson](https://mesonbuild.com/index.html)
to build. Meson can be obtained from your distro or PyPI.

After creating your build directory (e.g. `meson setup build`), you can view a list
of all the build options via `meson configure build`. You could also just simply
look at the `meson_options.txt` file. Logs are stored in `meson-logs` within
your build directory.

Example:

    meson setup build
    meson compile -C build
    meson install -C build

For libplacebo, meson can use a git check out as a subproject for a convenient
way to compile mpv if a sufficient libplacebo version is not easily available
in the build environment. It will be statically linked with mpv. Example:

    mkdir -p subprojects
    git clone https://code.videolan.org/videolan/libplacebo.git --depth=1 --recursive subprojects/libplacebo

Essential dependencies (incomplete list):

- gcc or clang
- X development headers (xlib, xrandr, xext, xscrnsaver, xpresent, libvdpau,
  libGL, GLX, EGL, xv, ...)
- Audio output development headers (libasound/ALSA, pulseaudio)
- FFmpeg libraries (libavutil libavcodec libavformat libswscale libavfilter
  and either libswresample or libavresample)
- libplacebo
- zlib
- iconv (normally provided by the system libc)
- libass (OSD, OSC, text subtitles)
- Lua (optional, required for the OSC pseudo-GUI and youtube-dl integration)
- libjpeg (optional, used for screenshots only)
- uchardet (optional, for subtitle charset detection)
- nvdec and vaapi libraries for hardware decoding on Linux (optional)

Libass dependencies (when building libass):

- gcc or clang, nasm on x86 and x86_64
- fribidi, freetype, fontconfig development headers (for libass)
- harfbuzz (required for correct rendering of combining characters, particularly
  for correct rendering of non-English text on macOS, and Arabic/Indic scripts on
  any platform)

FFmpeg dependencies (when building FFmpeg):

- gcc or clang, nasm on x86 and x86_64
- OpenSSL or GnuTLS (have to be explicitly enabled when compiling FFmpeg)
- libx264/libmp3lame/libfdk-aac if you want to use encoding (have to be
  explicitly enabled when compiling FFmpeg)
- For native DASH playback, FFmpeg needs to be built with --enable-libxml2
  (although there are security implications, and DASH support has lots of bugs).
- AV1 decoding support requires dav1d.
- For good nvidia support on Linux, make sure nv-codec-headers is installed
  and can be found by configure.

Most of the above libraries are available in suitable versions on normal
Linux distributions. For ease of compiling the latest git master of everything,
you may wish to use the separately available build wrapper ([mpv-build][mpv-build])
which first compiles FFmpeg libraries and libass, and then compiles the player
statically linked against those.

If you want to build a Windows binary, see [Windows compilation][windows_compilation].


## Release cycle

Once or twice a year, a release is cut off from the current development state
and is assigned a 0.X.0 version number. No further maintenance is done, except
in the event of security issues.

The goal of releases is to make Linux distributions happy. Linux distributions
are also expected to apply their own patches in case of bugs.

Releases other than the latest release are unsupported and unmaintained.

See the [release policy document][release-policy] for more information.

## Bug reports


Please use the [issue tracker][issue-tracker] provided by GitHub to send us bug
reports or feature requests. Follow the template's instructions or the issue
will likely be ignored or closed as invalid.

Questions can be asked in the [discussions][discussions] or on IRC (see
[Contact](#Contact) below).

## Contributing


Please read [contribute.md][contribute.md].

For small changes you can just send us pull requests through GitHub. For bigger
changes come and talk to us on IRC before you start working on them. It will
make code review easier for both parties later on.

You can check [the wiki](https://github.com/mpv-player/mpv/wiki/Stuff-to-do)
or the [issue tracker](https://github.com/mpv-player/mpv/issues?q=is%3Aopen+is%3Aissue+label%3Ameta%3Afeature-request)
for ideas on what you could contribute with.

## License

GPLv2 "or later" by default, LGPLv2.1 "or later" with `-Dgpl=false`.
See [details.](https://github.com/mpv-player/mpv/blob/master/Copyright)

## History

This software is based on the MPlayer project. Before mpv existed as a project,
the code base was briefly developed under the mplayer2 project. For details,
see the [FAQ][FAQ].

## Contact


Most activity happens on the IRC channel and the GitHub issue tracker.

- **GitHub issue tracker**: [issue tracker][issue-tracker] (report bugs here)
- **Discussions**: [discussions][discussions]
- **User IRC Channel**: `#mpv` on `irc.libera.chat`
- **Developer IRC Channel**: `#mpv-devel` on `irc.libera.chat`

[FAQ]: https://github.com/mpv-player/mpv/wiki/FAQ
[releases]: https://github.com/mpv-player/mpv/releases
[mpv-build]: https://github.com/mpv-player/mpv-build
[issue-tracker]:  https://github.com/mpv-player/mpv/issues
[discussions]: https://github.com/mpv-player/mpv/discussions
[release-policy]: https://github.com/mpv-player/mpv/blob/master/DOCS/release-policy.md
[windows_compilation]: https://github.com/mpv-player/mpv/blob/master/DOCS/compile-windows.md
[interface-changes]: https://github.com/mpv-player/mpv/blob/master/DOCS/interface-changes.rst
[api-changes]: https://github.com/mpv-player/mpv/blob/master/DOCS/client-api-changes.rst
[restore-old-bindings]: https://github.com/mpv-player/mpv/blob/master/etc/restore-old-bindings.conf
[restore-osc-bindings]: https://github.com/mpv-player/mpv/blob/master/etc/restore-osc-bindings.conf
[contribute.md]: https://github.com/mpv-player/mpv/blob/master/DOCS/contribute.md
