# Third-party notices

## CLI11

`dal_game` parses its command line and its `config.json` file with CLI11
(<https://github.com/CLIUtils/CLI11>), a header-only C++ library. It is a git submodule in
`externals/CLI11`, compiled into `dal_game`; nothing is copied into the build output.

| | |
| --- | --- |
| Version | CLI11 2.7.2 (tag `v2.7.2`, commit `cbd58a3696887b34c70949aef21a71735a0c2ad5`) |
| Source | <https://github.com/CLIUtils/CLI11/tree/v2.7.2> |
| Licence | BSD 3-Clause (below) |

BSD 3-Clause requires this notice to accompany binary redistributions. The licence text of
`externals/CLI11/LICENSE`:

```text
CLI11 2.7.2 Copyright (c) 2017-2026 University of Cincinnati, developed by Henry
Schreiner under NSF AWARD 1414736. All rights reserved.

Redistribution and use in source and binary forms of CLI11, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## RPCS3 `key_vault.h`

To decrypt the game's `.sdat` files, the build downloads `rpcs3/Crypto/key_vault.h` from the RPCS3
repository (<https://github.com/RPCS3/rpcs3>, GNU General Public License, version 2) at a pinned
commit into `generated/deps/rpcs3`, and `dal_sdat` reads four key constants from it. The file is
not part of this repository, is not compiled or linked, and is not copied into the build output.
The pin and its SHA-256 are in `cmake/sdat_keys.cmake`; see [SDAT files](docs/sdat.md).

## Microsoft GameInput

Gamepads are read with Microsoft's GameInput API (version 3). The `Microsoft.GameInput` NuGet
package is not part of this repository. `tools/fetch-gameinput.ps1` downloads the pinned package
from <https://www.nuget.org/packages/Microsoft.GameInput> into `generated/deps/gameinput` and
verifies its SHA-256 (`-From <nupkg>` takes a local copy and applies the same check); the pin is
in `cmake/gameinput.cmake`. The build uses only the package's `GameInput.h` and `GameInput.lib`;
nothing from the package is copied into the build output. The runtime is Microsoft's GameInput
redistributable, which the user installs from the package's `redist/GameInputRedist.msi`.

| | |
| --- | --- |
| Version | `Microsoft.GameInput` 3.5.283 (API version 3) |
| Source | <https://api.nuget.org/v3-flatcontainer/microsoft.gameinput/3.5.283/microsoft.gameinput.3.5.283.nupkg>; project page <https://aka.ms/gameinput> |
| Header and static library | MIT License, Copyright (c) Microsoft Corporation (`GameInput.h`, `GameInput.lib`, and `GameInput.cpp`, the source of the library) |
| Redistributable runtime | Microsoft Software License Terms, "Microsoft GameInput Redistributable" (the package's `LICENSE.txt`) |

### Licences

The public header and the static library are licensed under the MIT License; the text is in the
header and in `GameInput.cpp`. The package's `LICENSE.txt` is the licence of the redistributable
runtime (`GameInputRedist.dll`, `GameInputRedist.msi`), and `NOTICE.txt` lists the third-party
component that the package incorporates (Fusion, MIT License). This project neither copies nor
redistributes the runtime.

### Run-time loading

`GameInput.lib` is a static loader: it links no GameInput DLL, so the executables start on a machine
without a GameInput runtime. At the first GameInput call it uses the runtime installed on the
machine (the in-box `GameInput.dll` or the installed `GameInputRedist.dll`). If no runtime that
implements API version 3 is found, `dal_game` logs one warning that names `GameInputRedist.msi` and
the gamepads are disabled; the keyboard keeps working.

## FFmpeg

The runtime decodes the movies (H.264 video and ATRAC3plus audio) and the game's ATRAC3plus sound
(music and sound effects) with FFmpeg (`libavcodec`, `libavutil`, `libswscale`; `libavcodec`
also imports `libswresample`, none of whose functions the runtime calls). FFmpeg is not part of
this repository and no FFmpeg binary is tracked. `tools/fetch-ffmpeg.ps1` downloads a pinned,
SHA-256 verified build into `generated/deps/ffmpeg` (`-From <zip>` takes a local copy of the
pinned zip and applies the same check); the pin is in `cmake/ffmpeg.cmake`. The build copies the
four DLLs, the FFmpeg `LICENSE.txt` and this notice next to `dal_game` (`bin/` of the build
directory, the licence and notice under `bin/licenses/ffmpeg/`).

| | |
| --- | --- |
| Version | FFmpeg `n9.0.2-22-g46d8f462ee` (release/9.0 branch, 22 commits after tag `n9.0.2`) |
| Build | BtbN FFmpeg-Builds, `win64-lgpl-shared`, release `autobuild-2026-10-06-13-06` |
| Licence | GNU Lesser General Public License, version 3 or later (see below) |

### Licence

FFmpeg is licensed under the GNU Lesser General Public License, version 2.1 or later. This build
is configured with `--enable-version3`, so the resulting libraries are distributed under the LGPL
version 3 or later. It is not configured with `--enable-gpl` or `--enable-nonfree`, and no GPL or
non-free component is included. The full licence text ships with the build as `LICENSE.txt` and must
accompany any redistributed copy of the DLLs.

### Dynamic loading and replacement

The FFmpeg DLLs are not linked into the executables. The runtime loads them at run time with
`LoadLibraryExW` and the flags `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`,
from the directory of the executable, so their dependencies are looked up only in that directory
and in the Windows system directory (not in `PATH` or the working directory). They can be replaced
by another build of the same major versions (`avcodec-63`, `avutil-61`, `swresample-7`,
`swscale-10`) without rebuilding this project; a different major version is reported as
unavailable. Without usable DLLs the runtime logs two warnings,
`dal: sail: codecs unavailable: <reason>` and `dal: atrac: no decoder: <reason>`. Every movie then
plays as black frames without sound for the duration of its stream and ends normally, and the
game's music and sound effects (`cellAtrac`) are silent.

### Source code

- FFmpeg source at the exact commit of the build:
  <https://github.com/FFmpeg/FFmpeg/tree/46d8f462eeb87ee1f704d8c44a0ee24fca471ad1>
  (commit `46d8f462eeb87ee1f704d8c44a0ee24fca471ad1`).
- Build scripts (toolchain, patches and the configuration of every bundled library) at the tag of
  the pinned release:
  <https://github.com/BtbN/FFmpeg-Builds/tree/autobuild-2026-10-06-13-06>.
- The pinned release: <https://github.com/BtbN/FFmpeg-Builds/releases/tag/autobuild-2026-10-06-13-06>.

### Libraries bundled in the build

The `ffmpeg -version` configuration of the pinned build enables these external components, which
are statically included in the DLLs under their own licences (see the build scripts above): iconv,
zlib, libxml2, libvmaf, fontconfig, harfbuzz, freetype, fribidi, lcms2, Vulkan, dav1d, vorbis,
rav1e, librsvg, gmp, lzma, liblcevc-dec, OpenCL, AMF, aom, chromaprint, ffnvcodec, libgme,
kvazaar, aribb24, aribcaption, libass, libbluray, libjxl, mp3lame, opus, libplacebo, librist,
libssh, theora, vpx, webp, zmq, lv2, libvpl, OpenAL, liboapv, opencore-amr (narrowband and
wideband), openh264, openjpeg, openmpt, schannel, SDL2, snappy, soxr, srt, svtav1, twolame, uavs3d,
VA-API, VapourSynth, vvenc, zimg and zvbi. This project uses only the H.264 and ATRAC3plus decoders
and the colour conversion of `libswscale`.
