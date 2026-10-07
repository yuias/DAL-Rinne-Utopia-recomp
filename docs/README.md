# Development documentation

How to build and run the game is in the [top-level README](../README.md). These files describe
how the project works:

| File | Contents |
| --- | --- |
| [analysis-tools.md](analysis-tools.md) | ELF summary, SPU probe, disassembler, Ghidra cross-check, NID and syscall name tables, target pin |
| [function-recovery.md](function-recovery.md) | Recovering function starts and extents, the function database and its report |
| [emitter.md](emitter.md) | Turning the recovered functions into C, the contract header and the semantics harness |
| [runtime.md](runtime.md) | Runtime internals: logging, census, memory map, dispatch, threads, system calls, HLE imports, headless policy, PNG decoding, default handlers |
| [virtual-file-system.md](virtual-file-system.md) | Guest path mapping and the VFS configuration file |
| [rendering.md](rendering.md) | Fake RSX, the Vulkan renderer, and capture and replay |
| [movies.md](movies.md) | cellSail HLE, PAMF demuxing, FFmpeg decoding and `dal_moviedump` |
| [tests.md](tests.md) | CTest targets, the `game_boot` test and the Python unit tests |
| [layout.md](layout.md) | Repository layout |
| [porting-guide.md](porting-guide.md) | Applying the method to another PS3 game |
