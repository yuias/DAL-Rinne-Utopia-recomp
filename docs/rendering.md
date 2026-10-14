# RSX and rendering

## Fake RSX

`cellGcmSys` initialization creates the RSX state and starts two things that stand in for the
graphics hardware. Without `--render` (or `--rsx-capture`) the FIFO is only parsed and nothing is
drawn; with `--render vulkan` the same thread also feeds a renderer (see [Rendering](#rendering)).

- The RSX thread is a host thread without a guest context. It follows the `put` pointer that the
  game writes into the control register, parses the FIFO (`runtime/src/rsx/fifo_parser.cpp`:
  methods, increments, jumps, calls and returns, non-incrementing runs) and executes the
  synchronization methods: semaphores and labels, reports and timestamps, `ref`, the flip
  command and the user command. It then publishes `get`. Method semantics follow RPCS3's
  `Emu/RSX/NV47/HW` sources. Every register write is counted for the census, and the thread
  raises one vertical-blank event every 1/60 s, also while it works through a long stretch of
  commands. A flip from the command buffer completes where the command executes, as in RPCS3,
  and does not wait for a vertical blank. Where the game's
  frame rate comes from is described under [Rendering](#rendering).
- `_gcm_intr_thread` is a guest service thread, created as in RPCS3. The RSX thread queues the
  interrupts and this thread runs the handlers the game registered with
  `cellGcmSetVBlankHandler`, `cellGcmSetFlipHandler` and `cellGcmSetUserHandler`, with the guest
  lock held. A vblank is not queued while an earlier one is still waiting; flips and user
  commands are never dropped.

The method census (the `rsx` section of the census) lists the number of FIFO headers, the number
of distinct methods and flips, how often each interrupt handler ran, and the count per method,
named when the method has a known name. With the renderer on, it also gives the number of draws
rendered and skipped.

## Rendering

The renderer is on when `dal_game` shows its window (the default) and off with `--no-window`.
`--render vulkan` (implied by the window and by `--dump-frames`) creates it on the RSX thread when that thread starts; the RSX thread then hands each
frame's commands to it outside the guest lock, so rendering does not hold up guest threads. The
code is in `runtime/src/rsx` (the frame builder) and `runtime/src/render` (the renderer).

- **Frame builder** (`rsx_frame_builder.cpp`). It keeps a host copy of the RSX register file and the
  vertex program and constant stores (`rsx_registers.cpp`), applies every completed register write
  to it, and turns the stream into four kinds of plain commands: clear, draw, flip and copy (a render
  target copied into another one, see [Guest copies of render targets](#guest-copies-of-render-targets)). The state
  decoders (`rsx_decode.cpp`) read surface, blend, depth, texture, vertex and program state from
  the register file. Each command carries every guest byte it needs (vertex and index data,
  texture texels, shader ucode), so the renderer never touches guest memory. The texture bytes come
  from a cache shared between draws (see [Texture cache](#texture-cache-of-the-frame-builder)), so
  draws that sample the same unchanged texture share one copy. Draw boundaries,
  addressing and register meanings follow RPCS3's `Emu/RSX` sources. At a flip the registers are
  reset the way RPCS3 resets them, so the next frame starts from the same state.
- **Renderer** (`vk_renderer.cpp` and neighbours). Vulkan 1.3 with dynamic rendering, the loader
  through volk (`externals/volk`, loaded at run time, so no import library is linked) and memory
  through VulkanMemoryAllocator (`externals/VulkanMemoryAllocator`). Colour and depth surfaces are
  kept in a render target cache keyed by memory location and offset. A texture whose address is
  a known render target is sampled from that target (render target aliasing), and a draw that
  samples the target it renders to reads a copy of it. Other textures are uploaded from the
  guest bytes into B8G8R8A8 images (linear A8R8G8B8) or BC1/BC2/BC3 images (DXT1/DXT23/DXT45,
  one 2D mip level, block rows copied as stored whatever the layout bit says), and only when
  their content changes: a draw whose texture the image already holds does not upload it again. A
  unit without a usable texture reads a 1x1 magenta image.
  Cull mode, depth state, viewport, scissor, topology and blend constants are dynamic state;
  pipelines are cached by what is baked into them.
- **Readback** is gated. Copying the display buffer back to the host costs 8 MiB per flip at 1080p,
  so a live run does it only for flips that are dumped (`--dump-frames`). `dal_rsxreplay` always
  reads the last flip back.
- **Shader translation** (`vp_translator.cpp`, `fp_translator.cpp`). Vertex and fragment programs
  are translated from RSX microcode to GLSL 4.50 and compiled with shaderc (the SDK's static
  library) to SPIR-V 1.6 for Vulkan 1.3, without optimisation. The result is cached on disk in
  `generated/shader_cache` (`--shader-cache DIR` changes it) as
  `<stage>_<program hash>_<GLSL hash>_<translator version>.spv`. A missing or corrupt file is
  recompiled and rewritten, and the translator version in the name keeps SPIR-V from an older
  translation from being reused. A program with an opcode that is not translated skips its draw
  and is counted.
- **Flips and vsync.** A flip is done when the command executes (see [Fake RSX](#fake-rsx)). With the window
  the displayed buffer is also blitted, letterboxed to 16:9, to a swapchain with the FIFO present
  mode, so the display's refresh is what paces the RSX thread; presenting runs outside the guest
  lock. Without a window nothing waits for a vertical blank in the renderer, and the game's own
  waits on the 60 Hz vblank handler set its frame rate. While the game is fast-forwarded (see
  [Guest time and fast-forward](runtime.md#guest-time-and-fast-forward)) the vblank period is divided
  by the guest speed, and the swapchain is recreated with the `MAILBOX` present mode (else
  `IMMEDIATE`), so the window no longer paces the RSX thread: a flip with no swapchain image free
  without waiting is rendered and submitted without a present and counted as `skipped` in the
  `dal: render: window:` debug line. That line also reports the time the renderer waited for a frame
  slot's fence (`fence wait avg ... max ... ms`), which is where a `FIFO` swapchain holds the thread.
  The [debug overlay](runtime.md#debug-overlay) is drawn on the swapchain image after that blit, in a
  separate pass before the image is presented. It is not drawn into the render target, so it never
  appears in `--dump-frames` files, requested PNG dumps, the display readback or captures, and it only
  redraws when a flip completes (while fast-forwarding, when a flip is presented).
- **`rsx unimplemented` census.** The decoders, the frame builder and the renderer count what they
  meet but cannot handle yet (surface, depth and texture formats, texture layouts, vertex types,
  primitives, vertex and fragment opcodes, blend modes, other state and unknown methods) under
  `<category> <key>`. The stop census prints them as a `census: rsx unimplemented N kinds`
  section, most frequent first (omitted when empty). It is the work list for later scenes: a draw
  that depends on one of them is skipped or drawn incompletely, and the `skipped` count in the
  `rsx` section says which.

### Texture cache of the frame builder

Copying every enabled texture out of guest memory for every draw, and uploading it again, dominated the
cost of UI screens: the OPTION menu draws about 100 times per frame with the same two unchanged textures,
and it ran at 15 flips and 30 vertical blanks per second until the builder kept the bytes (`rsx_texture_cache.cpp`)
and the renderer uploaded them once. With the cache it runs at 30 and 60, like the title menu.

- *Key.* Everything that decides the copied bytes: location, offset, width, height, the pitch register
  (before the 0 -> row size fix-up) and the raw format byte (it carries the linear and unnormalized flags).
  Two descriptors that differ in any of them get separate entries; the filter, address, remap and border
  words only affect sampling and are not part of the key. A texture at the address of a known render target
  is an alias and never reaches the cache.
- *Entry.* The blob (the raw guest bytes, rows at the pitch; the renderer still byte swaps A8R8G8B8 on
  upload), a content id, a 64-bit hash of those bytes (`rsxHashBytes`, four multiply-fold lanes over 64
  bytes per step), the guest range and the pitch to hand out. The content id comes from a process-wide
  counter, so two blobs never share an id, whichever builder made them. A draw carries the blob and the id in
  its `RsxTextureDesc`, and keeps the blob alive after the cache drops or replaces it.
- *Epochs.* Guest memory can change between draws and the builder cannot see it, so an entry is trusted only
  inside an epoch, a stretch of the command stream with no synchronization point. A new epoch starts at every
  flip and at `NV406E_SEMAPHORE_ACQUIRE`, `NV406E_SEMAPHORE_RELEASE`, `NV406E_SET_REFERENCE`,
  `NV4097_GET_REPORT`, `NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE`, `NV4097_TEXTURE_READ_SEMAPHORE_RELEASE`
  and the two registers of `GCM_SET_USER_COMMAND`: the points where the guest can learn how far the RSX
  has got. It also starts at `NV3089_IMAGE_IN` and `NV0039_BUFFER_NOTIFY`, which this build does not execute
  but which would let the RSX write memory. A game has to synchronize with the RSX before it reuses
  texture memory the RSX may still read, so a change it makes behind the RSX's back shows up at the next of
  these points. Inside an epoch an entry is used without reading guest memory. On the first use in a
  later epoch the guest range is read into a scratch buffer and hashed; an unchanged hash keeps the blob and
  its id, a changed one stores a new blob under a new id.
- *Writes by the RSX.* A clear with color bits or a draw with a color write mask, to a color surface, ends the
  trust in every entry whose range overlaps the surface, at once and without a new epoch (a surface with no
  pitch or height starts a new epoch). Each `NV308A_COLOR` word of an inline blit does the same for its
  4 destination bytes, which the RSX thread writes into guest memory, so only a texture the word overlaps is
  read again. Such an entry is read and hashed again at its next use. The renderer keeps surface contents on
  the GPU and never writes them back, so the guest bytes are normally unchanged and the hash keeps the blob.
  A guest copy of a surface's bytes reads those stale bytes too; see
  [Guest copies of render targets](#guest-copies-of-render-targets).
  Depth and stencil writes are not tracked: the builder keeps no depth surface and guest memory under
  one never changes. More than 64 distinct written ranges in one epoch start a new epoch.
- *Bounds.* Blobs are kept up to 256 MiB; past that the least recently used entries go. Every 32 flips,
  entries unused for 600 flips are dropped. A texture larger than the per-texture limit is counted as
  `texture layout too large` and gets no bytes.
- *Limitation.* A guest write that is not followed by one of the synchronization points before the RSX
  consumes the draw is not seen until the next epoch. Progress the guest learns only by polling the `get`
  word of the control register is not a synchronization point, and nothing watches guest pages.

The renderer uploads from a blob only when the image does not hold it already: each `TextureImage` remembers
the content id it last received, and `prepareTextures` skips the upload when the id matches (an id of 0,
used by hand-made descriptors, always uploads; an image re-created after the image cache trimmed it starts
without an id). `RendererStats::texture_uploads` counts the uploads. An upload that replaces content
mid-frame is ordered after the draws recorded earlier by the image barrier of the upload, whose source scope
is all commands.

At stop, `dal_game` logs one line with the counters, for example:

```
dal: rsx: texture cache: 6426 epochs, 54167 hits, 2849 checks (9366.3 MiB), 127 new blobs (445.5 MiB), 4 entries (12.7 MiB)
```

A hit is a use served without reading guest memory. A check is a guest read plus a hash (the first use in an
epoch, or after an RSX write), with the bytes it read in parentheses. A new blob is a changed or first-seen
texture, with the bytes it stored. The last figures are the entries and bytes still cached. About one
check per sampled texture per flip is expected; a check per draw means texture memory overlaps a render
target that every draw writes.

### Guest copies of render targets

The renderer keeps render target pixels on the GPU only, so the guest bytes of a surface are stale. That
matters when the game copies them with the CPU and then draws the copy as a texture: during dialogue it
snapshots the character layer into a new buffer before each expression crossfade and draws that buffer as
the fading-out old expression. Read from guest memory, the buffer held whatever was there before (title
screen art), and that flashed over the scene. Writing the pixels back to guest memory is not done; the copy
is made on the GPU instead.

- *The override.* The game's statically linked `memmove` is overridden by the `renderTargetCopy` handler
  (see [Overrides](overrides.md#game-handlers)). It reads destination, source and byte count, runs the
  original copy so guest memory stays exactly as it was, and then calls `rsxNoteGuestCopy`. That function
  returns at once unless the source lies in RSX local memory, so the roughly 5.95 million calls of a 200 s
  run cost almost nothing; for the others it checks that the destination is local or IO-mapped main memory
  and queues the request (`RsxConsumer::queueGuestCopy`, at most 64 pending, past that the oldest is dropped
  and counted as `state guest copy queue full`). The handler notes the copy after the original because
  the original may give up the guest lock (at loop back edges); noted earlier, the RSX thread could hash a
  half-copied destination.
- *Position in the stream.* `queueGuestCopy` tags each request with the `put` the guest had written when it
  made the copy, and the copy is applied where the parser reaches that position: `FifoParser::run` takes a
  stop position (`kFifoNoStop` for none) and ends the run, at a command boundary, when `get` equals it.
  So the copy lands after every command the guest had submitted by then and before everything it submits
  later, which is what the guest's bytes reflect on hardware once the RSX has executed the submitted work.
  In this game `put` at the call lies past the frame's whole redraw of the layer and its composite passes,
  just before the frame's last display draw and flip. The consumer's current position is not used: the RSX
  thread leaves the guest lock after every draw end and every hold budget, so at the call it can stand
  inside a layer's redraw (after the layer's clear, before most of its draws), and a copy applied there
  copied a half-drawn layer. A copy tagged behind a pending draw waits for that draw to be built first.
  A tag that the parser passes inside a command or never meets is applied once the stream is fully consumed
  and counted as `state guest copy position missed`; late is better than never. A copy therefore waits
  until the stream reaches its tag, at the latest the next `pump()` after the guest puts more commands or
  the next vertical blank. Without a frame builder the queue is discarded.
- *The command.* `RsxFrameBuilder::onGuestCopy` accepts a copy only when the source is the first byte of a
  known color surface and the copy covers the surface (`pitch * (height - 1) + width * 4` bytes); the
  destination gets the source's size, pitch and format A8R8G8B8. It then emits `RsxCopy`, and the renderer
  copies the source render target's image into a render target at the destination with `vkCmdCopyImage` over
  the common extent (`RendererStats::target_copies`). A source the renderer does not know yet leaves the
  destination cleared to transparent black and is counted as `state render target copy from an unknown
  target`. When a copy is made, the consumer logs
  `dal: rsx: render target copy in frame N: local 0x... -> local 0x... (0x... bytes)`, and the stop line
  `dal: rsx: frame builder: N clears, N draws, N flips, N copies` counts them.
- *The alias.* The destination is registered as a known surface marked `copied`, so a later texture at its
  address is sampled from the copied render target (render target aliasing) instead of the guest bytes. The
  alias holds only while the guest bytes under the destination hash to the value taken right after the copy
  (`copy_hash`). They are read and hashed again at the first use in each epoch (see
  [Texture cache](#texture-cache-of-the-frame-builder)); a different hash marks the surface `copy_stale` and
  the texture is read from guest memory again, as the game has reused the buffer. A new copy to the same
  address replaces the entry in place and revives it. A draw or clear that writes the surface makes it an
  ordinary render target (`copied` is cleared), because the RSX now owns its contents. The game makes a new
  destination per use, so when the 64-entry table of known surfaces is full a new destination reuses the slot
  of a stale copied surface, else of the one copied longest ago; surfaces the RSX drew are never evicted.
- *Not handled.* Copies that are not a whole surface from its first byte (sub-rectangles, row-by-row copies,
  other pitches or formats, sources in main memory) are not turned into commands; one that touches the bytes
  of a known surface without covering it is counted as `state guest copy of part of a render target`, and a
  copy onto the surface itself as `state guest copy onto its own render target`. Any other CPU read of a
  render target still sees the stale bytes. An inline blit into a copy destination is seen only at the next
  epoch, as for any texture.

### Capture and replay

`--rsx-capture N[,M...]` records the RSX inputs of the given frames: the numbers are 1-based flip
numbers, and each capture is written when its frame's flip completes, to
`generated/rsx/captures/frame_<N>.dalcap` (relative to the working directory). A capture holds the
register writes from the previous flip to this one, the guest bytes the frame builder read while
handling them, and the register and surface state the frame started from. The builder reads a texture at
its first use in an epoch, and the flip that begins a captured frame starts an epoch, so a capture records
each texture at its first use in the frame and the replay, which starts with an empty cache, makes the same
read. It needs no renderer and no window. Captures are derived from the game, so they are git-ignored with the rest of
`generated/`.

Guest copies of render targets (see above) are part of the stream. The capture file is version 3 (the
reader still accepts versions 1 and 2). A `COPY` chunk holds each copy request the consumer applied, in
stream order with the register writes and the guest bytes (the builder reads the destination bytes before
the chunk), and replay hands it to the builder at the same point, which decides again from the recorded
bytes. The chunk is recorded where the copy is applied, not where the game made it; in this game that is
in the capture of the frame that drew the source. A copy applied in a later frame's capture would find no
render target for the source in a replay, which leaves the destination transparent black (counted as
`state render target copy from an unknown target`). The surface records of the start state carry the copy fields of
the known surfaces (`copied`, `copy_stale` and the hash); version 1 and 2 files have none. A replay has no
GPU contents from before the captured frame: in a frame after the copy, the texture is still an alias of
the copied surface, but that surface shows transparent black (counted as `texture alias of unknown render
target`). A copy whose source was drawn earlier in the captured frame is made for real.

`dal_rsxreplay` replays a capture through the frame builder and the renderer without the game:

```sh
dal_rsxreplay CAPTURE [--out DIR] [--compare PNG [--max-diff N] [--max-mean X] [--max-differing P]]
                      [--no-validation] [--shader-cache DIR]
```

| Option | Meaning |
| --- | --- |
| `CAPTURE` | A `.dalcap` file. |
| `--out DIR` | Write the displayed frame of the last flip as `DIR/<capture name>.png`. |
| `--compare PNG` | Compare that frame with a reference image. The sizes must match. |
| `--max-diff N` | With `--compare`: fail when the largest per-channel difference exceeds `N`. |
| `--max-mean X` | With `--compare`: fail when the mean per-channel difference exceeds `X`. |
| `--max-differing P` | With `--compare`: fail when more than `P` percent of the pixels differ (by more than 2 in any channel). |
| `--no-validation` | Do not enable the Vulkan validation layer, which is on by default. |
| `--shader-cache DIR` | SPIR-V cache directory. Without it nothing is cached. |

The thresholds are optional and unchecked unless given, and they need `--compare`. The tool prints
the commands the frame builder produced (`clear`, `draw`, `flip`, and `copy 1280x720@0x3420000 -> @0x9020300`
for a render target copy: source width, height and offset, then the destination offset), then a summary line
`replay: N commands, N draws, N skipped` (with the diff figures when comparing), the
`rsx unimplemented` section (`census: rsx unimplemented 0 kinds` when it is empty), the path of
the PNG, and `validation: active|inactive|disabled, N errors`. The exit code is 1 when a
threshold is exceeded, a file cannot be read or written, there is no frame to display or the
validation layer reported an error, and 2 for a usage error. Without a usable Vulkan device it
still prints the command list and counts every draw as skipped.

The CTest entry `rsx_replay` runs `dal_rsxreplay` on `generated/rsx/captures/title.dalcap` with
the options pinned in `cmake/rsx_replay.cmake`: a SHA-256 for the reference image
`generated/rsx/reference/title.png` and the thresholds (largest difference 8, at most 0.5 percent
of the pixels differing). Both files are game-derived and git-ignored, so the test is registered
only when the capture exists and the reference exists and matches the pinned hash; configuration
prints the reason otherwise. It passes when the summary line is printed and the validation layer
reports no error. To make a capture, run `dal_game` with `--rsx-capture` on the frame you want
and copy the file to `generated/rsx/captures/title.dalcap`.

Three further renderer tests are registered: `render_core` (renders synthetic command lists, and
skips itself when there is no Vulkan device), and `vp_translator` and `fp_translator` (need no
GPU; they also check programs dumped under `generated/rsx` and skip those when absent).
