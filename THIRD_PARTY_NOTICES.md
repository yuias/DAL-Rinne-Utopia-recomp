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

## Other libraries compiled into the executables

These libraries are git submodules in `externals/` and are compiled from source into the
executables; none of them is copied into the build output as a separate file. Each licence
asks for its notice to accompany binary redistributions, so the texts follow.

### spdlog

The runtime's logging. It is built with `SPDLOG_USE_STD_FORMAT`, so the fmt library bundled in
spdlog is not compiled.

| | |
| --- | --- |
| Version | tag `v1.17.0`, commit `79524ddd08a4ec981b7fea76afd08ee05f83755d` |
| Source | <https://github.com/gabime/spdlog/tree/v1.17.0> |
| Licence | MIT License |

The licence text of `externals/spdlog/LICENSE`:

````text
The MIT License (MIT)

Copyright (c) 2016 - present, Gabi Melman and spdlog contributors.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

-- NOTE: Third party dependency used by this software --
This software depends on the fmt lib (MIT License),
and users must comply to its license: https://raw.githubusercontent.com/fmtlib/fmt/master/LICENSE
````

### libpng

Decodes the game's PNG images (`cellPngDec`) and writes frame dumps.

| | |
| --- | --- |
| Version | tag `v1.6.59`, commit `cd952f49f95bb27154ae77dbb103032d95f6e580` |
| Source | <https://github.com/pnggroup/libpng/tree/v1.6.59> |
| Licence | PNG Reference Library License version 2 |

The licence text of `externals/libpng/LICENSE`:

````text
COPYRIGHT NOTICE, DISCLAIMER, and LICENSE
=========================================

PNG Reference Library License version 2
---------------------------------------

 * Copyright (c) 1995-2026 The PNG Reference Library Authors.
 * Copyright (c) 2018-2026 Cosmin Truta.
 * Copyright (c) 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson.
 * Copyright (c) 1996-1997 Andreas Dilger.
 * Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.

The software is supplied "as is", without warranty of any kind,
express or implied, including, without limitation, the warranties
of merchantability, fitness for a particular purpose, title, and
non-infringement.  In no event shall the Copyright owners, or
anyone distributing the software, be liable for any damages or
other liability, whether in contract, tort or otherwise, arising
from, out of, or in connection with the software, or the use or
other dealings in the software, even if advised of the possibility
of such damage.

Permission is hereby granted to use, copy, modify, and distribute
this software, or portions hereof, for any purpose, without fee,
subject to the following restrictions:

 1. The origin of this software must not be misrepresented; you
    must not claim that you wrote the original software.  If you
    use this software in a product, an acknowledgment in the product
    documentation would be appreciated, but is not required.

 2. Altered source versions must be plainly marked as such, and must
    not be misrepresented as being the original software.

 3. This Copyright notice may not be removed or altered from any
    source or altered source distribution.


PNG Reference Library License version 1 (for libpng 0.5 through 1.6.35)
-----------------------------------------------------------------------

libpng versions 1.0.7, July 1, 2000, through 1.6.35, July 15, 2018 are
Copyright (c) 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson, are
derived from libpng-1.0.6, and are distributed according to the same
disclaimer and license as libpng-1.0.6 with the following individuals
added to the list of Contributing Authors:

    Simon-Pierre Cadieux
    Eric S. Raymond
    Mans Rullgard
    Cosmin Truta
    Gilles Vollant
    James Yu
    Mandar Sahastrabuddhe
    Google Inc.
    Vadim Barkov

and with the following additions to the disclaimer:

    There is no warranty against interference with your enjoyment of
    the library or against infringement.  There is no warranty that our
    efforts or the library will fulfill any of your particular purposes
    or needs.  This library is provided with all faults, and the entire
    risk of satisfactory quality, performance, accuracy, and effort is
    with the user.

Some files in the "contrib" directory and some configure-generated
files that are distributed with libpng have other copyright owners, and
are released under other open source licenses.

libpng versions 0.97, January 1998, through 1.0.6, March 20, 2000, are
Copyright (c) 1998-2000 Glenn Randers-Pehrson, are derived from
libpng-0.96, and are distributed according to the same disclaimer and
license as libpng-0.96, with the following individuals added to the
list of Contributing Authors:

    Tom Lane
    Glenn Randers-Pehrson
    Willem van Schaik

libpng versions 0.89, June 1996, through 0.96, May 1997, are
Copyright (c) 1996-1997 Andreas Dilger, are derived from libpng-0.88,
and are distributed according to the same disclaimer and license as
libpng-0.88, with the following individuals added to the list of
Contributing Authors:

    John Bowler
    Kevin Bracey
    Sam Bushell
    Magnus Holmgren
    Greg Roelofs
    Tom Tanner

Some files in the "scripts" directory have other copyright owners,
but are released under this license.

libpng versions 0.5, May 1995, through 0.88, January 1996, are
Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.

For the purposes of this copyright and license, "Contributing Authors"
is defined as the following set of individuals:

    Andreas Dilger
    Dave Martindale
    Guy Eric Schalnat
    Paul Schmidt
    Tim Wegner

The PNG Reference Library is supplied "AS IS".  The Contributing
Authors and Group 42, Inc. disclaim all warranties, expressed or
implied, including, without limitation, the warranties of
merchantability and of fitness for any purpose.  The Contributing
Authors and Group 42, Inc. assume no liability for direct, indirect,
incidental, special, exemplary, or consequential damages, which may
result from the use of the PNG Reference Library, even if advised of
the possibility of such damage.

Permission is hereby granted to use, copy, modify, and distribute this
source code, or portions hereof, for any purpose, without fee, subject
to the following restrictions:

 1. The origin of this source code must not be misrepresented.

 2. Altered versions must be plainly marked as such and must not
    be misrepresented as being the original source.

 3. This Copyright notice may not be removed or altered from any
    source or altered source distribution.

The Contributing Authors and Group 42, Inc. specifically permit,
without fee, and encourage the use of this source code as a component
to supporting the PNG file format in commercial products.  If you use
this source code in a product, acknowledgment is not required but would
be appreciated.
````

### zlib

The compression library that libpng uses.

| | |
| --- | --- |
| Version | tag `v1.3.2`, commit `da607da739fa6047df13e66a2af6b8bec7c2a498` |
| Source | <https://github.com/madler/zlib/tree/v1.3.2> |
| Licence | zlib License |

The licence text of `externals/zlib/LICENSE`:

````text
Copyright notice:

 (C) 1995-2026 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
````

### yyjson

Reads the runtime's JSON files, including `config.json`.

| | |
| --- | --- |
| Version | tag `0.13.0`, commit `6447536015f3d600f3d65323b10976103b337ca7` |
| Source | <https://github.com/ibireme/yyjson/tree/0.13.0> |
| Licence | MIT License |

The licence text of `externals/yyjson/LICENSE`:

````text
MIT License

Copyright (c) 2020 YaoYuan <ibireme@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
````

### Vulkan-Headers

The Vulkan API headers that the renderer is compiled against.

| | |
| --- | --- |
| Version | tag `v1.4.363`, commit `6802bb4733b63ed5efd3adb308a6c885ef180ea1` |
| Source | <https://github.com/KhronosGroup/Vulkan-Headers/tree/v1.4.363> |
| Licence | Apache License 2.0 or MIT License, at the user's choice |

The licence text of `externals/Vulkan-Headers/LICENSE.md`:

````text
Copyright 2015-2026 The Khronos Group Inc.

Files in this repository fall under one of these licenses:

- `Apache-2.0`
- `MIT`

Note: With the exception of `parse_dependency.py`, which is based on an
external file, files using the `MIT` license also fall under `Apache-2.0`.
Example:

```
SPDX-License-Identifier: Apache-2.0 OR MIT
```

Full license text of these licenses is available at:

  * Apache-2.0: https://opensource.org/licenses/Apache-2.0
  * MIT: https://opensource.org/licenses/MIT
````

This project uses the headers under the MIT License, whose text is in
`externals/Vulkan-Headers/LICENSES/MIT.txt`.

### volk

Loads the Vulkan entry points at run time; `volk.c` is compiled into the renderer.

| | |
| --- | --- |
| Version | tag `vulkan-sdk-1.4.363.0`, commit `54fc0d7a72887cbfc5b030d26897b2b40ac53fb8` |
| Source | <https://github.com/zeux/volk/tree/vulkan-sdk-1.4.363.0> |
| Licence | MIT License |

The licence text of `externals/volk/LICENSE.md`:

````text
Copyright (c) 2018-2026 Arseny Kapoulkine

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
````

### Vulkan Memory Allocator

Allocates the renderer's Vulkan memory.

| | |
| --- | --- |
| Version | tag `v3.4.0`, commit `3aa921224c154a0d2c43912bc88e1c42ce1f7607` |
| Source | <https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tree/v3.4.0> |
| Licence | MIT License |

The licence text of `externals/VulkanMemoryAllocator/LICENSE.txt`:

````text
Copyright (c) 2017-2026 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
````

## RPCS3 key files

To decrypt the game's `EBOOT.BIN` and `.sdat` files, `dal_decrypt` and `dal_sdat` need keys. They
read them from two source files of RPCS3, <https://github.com/RPCS3/rpcs3>, licensed under the GNU
General Public License, version 2:

| | |
| --- | --- |
| Commit | `f9b8c209a3800c1865bcfeade611f65f5237138c` |
| `rpcs3/Crypto/key_vault.h` | SHA-256 `d5d0cede74c7900bb23ca8d60066194648ed4b976eaee60315c0bcbed7dc2aaf` |
| `rpcs3/Crypto/key_vault.cpp` | SHA-256 `a2947ff6b681258c198552ec6344e16685e2df649018206daa3f41a0a1aadbd7` |

The files are downloaded at configuration time into `generated/deps/rpcs3/<commit>/`, and their
SHA-256 is checked (the pins are in `cmake/rpcs3_keys.cmake`). Alternatively `DAL_RPCS3_CRYPTO_DIR`
(`-Rpcs3CryptoDir`) names the `rpcs3/Crypto` directory of a local RPCS3 checkout. The tools read only
the key constants from the files, as data. They are not compiled, linked, committed or
redistributed, and no key appears in this repository or in any build output other than the
decrypted files themselves. See [Building](docs/building.md#keys) and [SDAT files](docs/sdat.md).

## Microsoft GameInput

On Windows, gamepads are read with Microsoft's GameInput API (version 3). On macOS this package is
not used: gamepads are read through Apple's GameController.framework, a system framework. The
`Microsoft.GameInput` NuGet package is not part of this repository. `tools/fetch-gameinput.ps1` downloads the pinned package
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
this repository and no FFmpeg binary is tracked. On Windows, `tools/fetch-ffmpeg.ps1` downloads a
pinned, SHA-256 verified build into `generated/deps/ffmpeg` (`-From <zip>` takes a local copy of the
pinned zip and applies the same check); the pin is in `cmake/ffmpeg.cmake`. The build copies the
four DLLs, the FFmpeg `LICENSE.txt` and this notice next to `dal_game` (`bin/` of the build
directory, the licence and notice under `bin/licenses/ffmpeg/`). Unless it says macOS,
this section describes that Windows distribution; "FFmpeg on macOS" at its end covers the Mac.

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

### FFmpeg on macOS

On macOS the build uses no pinned FFmpeg and downloads none. The runtime loads the user's own
Homebrew FFmpeg (`brew install ffmpeg`) in place: `libavutil`, `libswresample`, `libswscale` and
`libavcodec` as `lib<name>.<major>.dylib`, opened with `dlopen` by absolute path from the Homebrew
`lib` directory that was compiled in. Nothing is linked, copied into the build output or
redistributed. Homebrew's FFmpeg is a GPL-configured build (it includes x264 and x265), unlike the
pinned LGPL build of the Windows distribution. This project loads it from the user's own
installation and never copies, bundles or redistributes it. The runtime uses only the H.264 and ATRAC3plus decoders and the conversion functions of
`libswscale`. See [Movies](docs/movies.md#ffmpeg-on-macos).

## Dear ImGui

The in-window debug overlay (`F1`, see [Debug overlay](docs/runtime.md#debug-overlay)) is drawn with
Dear ImGui, <https://github.com/ocornut/imgui>. It is a git submodule (`externals/imgui`) and is
built from source as a static library into `dal_game` and the overlay tests: the core files plus
the Vulkan backend and, on Windows, the Win32 backend or, on macOS, the Cocoa backend
(`imgui_impl_osx.mm`, compiled through `runtime/src/overlay/imgui_osx_bridge.mm`), without the
demo window. The Vulkan backend loads its entry points
through volk. The overlay uses ImGui's built-in default font, so no font file is involved.

| | |
| --- | --- |
| Version | Dear ImGui 1.91.9b (tag `v1.91.9b`, commit `f5befd2d29e66809cd1110a152e375a7f1981f06`) |
| Source | <https://github.com/ocornut/imgui/tree/v1.91.9b> |
| Licence | MIT License, Copyright (c) 2014-2025 Omar Cornut |

The licence text is also in `externals/imgui/LICENSE.txt`:

```text
The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
