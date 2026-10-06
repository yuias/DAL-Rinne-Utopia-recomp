// HLE handlers of the cellPngDec imports: Create, Destroy, ExtOpen, ExtReadHeader, ExtSetParameter,
// ExtDecodeData and Close. Behaviour follows RPCS3 `Emu/Cell/Modules/cellPngDec.cpp` (function of
// the same name): handle and stream objects are allocated through the guest's malloc/free callbacks,
// ReadHeader reports IHDR plus the ancillary chunks seen so far, SetParameter picks the output
// format and DecodeData writes rows at outputBytesPerLine intervals. Handlers run under the guest
// lock, so the handle and stream tables need no mutex.
//
// The codec is lodepng, which is not progressive: a stream is held as one host buffer. A source in
// guest memory is copied at open; a file source is read through the VFS. When cbCtrlStrm names a
// callback, DecodeData (and ReadHeader, if the first buffer lacks IHDR) calls it for more data
// until the IEND chunk is complete, then decodes the whole image.
//
// Colour handling maps RPCS3's libpng transforms onto lodepng conversions: palette, tRNS and
// sub-8-bit samples expand when the output is RGB/RGBA/ARGB, 16-bit samples are stripped to their
// high byte for an 8-bit output, ARGB is RGBA with the alpha moved to the front, and FIX_ALPHA
// overwrites the alpha only when the input has none.
//
// Deviations from RPCS3: every `throw_exception` site returns a logged error code instead; RGB to
// grayscale and palette-to-palette conversions that change the bit depth are refused with
// CELL_PNGDEC_ERROR_ARG; partial output through cbCtrlDisp is refused; outputPackFlag and
// commandPtr are ignored; unknown ancillary chunks are counted but their contents are not exposed
// (the cellPngDecGet* calls are not implemented).

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/guest_memory.h"
#include "core/log.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lodepng.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"
#include "lv2/vfs.h"

namespace dal {
namespace {

constexpr char kModule[] = "cellPngDec";

constexpr std::uint32_t kErrorHeader = 0x80611201;
constexpr std::uint32_t kErrorStreamFormat = 0x80611202;
constexpr std::uint32_t kErrorArg = 0x80611203;
constexpr std::uint32_t kErrorSeq = 0x80611204;
constexpr std::uint32_t kErrorFatal = 0x80611206;
constexpr std::uint32_t kErrorOpenFile = 0x80611207;

constexpr std::uint32_t kCodecVersion = 0x00420000;  // PNGDEC_CODEC_VERSION

// CellPngDecColorSpace.
constexpr std::uint32_t kGrayscale = 1;
constexpr std::uint32_t kRgb = 2;
constexpr std::uint32_t kPalette = 4;
constexpr std::uint32_t kGrayscaleAlpha = 9;
constexpr std::uint32_t kRgba = 10;
constexpr std::uint32_t kArgb = 20;

constexpr std::uint32_t kSrcFile = 0;    // CELL_PNGDEC_FILE
constexpr std::uint32_t kSrcBuffer = 1;  // CELL_PNGDEC_BUFFER
constexpr std::uint32_t kPackOneBytePerPixel = 1;
constexpr std::uint32_t kFixAlpha = 1;
constexpr std::uint32_t kLineMode = 1;  // CELL_PNGDEC_LINE_MODE
constexpr std::uint32_t kBottomToTop = 1;
constexpr std::uint32_t kStatusFinish = 0;
constexpr std::uint32_t kStatusStop = 1;

// Guest structure offsets (big-endian, 32-bit pointers).
//   CellPngDecThreadInParam: spuThreadEnable, ppuThreadPriority, spuThreadPriority, mallocFunc,
//   mallocArg, freeFunc, freeArg.
constexpr std::uint32_t kThreadInMallocFunc = 12;
constexpr std::uint32_t kThreadInMallocArg = 16;
constexpr std::uint32_t kThreadInFreeFunc = 20;
constexpr std::uint32_t kThreadInFreeArg = 24;
constexpr std::uint32_t kThreadInSize = 28;
constexpr std::uint32_t kThreadOutSize = 4;
//   CellPngDecSrc: srcSelect, fileName, fileOffset (s64), fileSize, streamPtr, streamSize, spu.
constexpr std::uint32_t kSrcFileName = 4;
constexpr std::uint32_t kSrcFileOffset = 8;
constexpr std::uint32_t kSrcFileSize = 16;
constexpr std::uint32_t kSrcStreamPtr = 20;
constexpr std::uint32_t kSrcStreamSize = 24;
constexpr std::uint32_t kSrcSize = 32;
//   CellPngDecCbCtrlStrm: func, arg.  CellPngDecOpnParam: selectChunk.
constexpr std::uint32_t kCbCtrlStrmSize = 8;
//   CellPngDecInfo.
constexpr std::uint32_t kInfoSize = 28;
//   CellPngDecInParam: commandPtr, outputMode, outputColorSpace, outputBitDepth, outputPackFlag,
//   outputAlphaSelect, outputColorAlpha.
constexpr std::uint32_t kInParamSize = 28;
//   CellPngDecOutParam: outputWidthByte (u64), outputWidth, outputHeight, outputComponents,
//   outputBitDepth, outputMode, outputColorSpace, useMemorySpace.
constexpr std::uint32_t kOutParamSize = 36;
//   CellPngDecExtInParam: bufferMode, outputCounts, spuMode.  CellPngDecExtOutParam: u64, u32.
constexpr std::uint32_t kExtInParamSize = 12;
constexpr std::uint32_t kExtOutParamSize = 12;
//   CellPngDecDataOutInfo: chunkInformation, numText, numUnknownChunk, status.
constexpr std::uint32_t kDataOutInfoSize = 16;

// Sizes of the guest blocks the object allocations ask for. The game never reads them; they only
// have to be distinct, writable guest memory.
constexpr std::uint32_t kHandleBlockSize = 0x10;
constexpr std::uint32_t kStreamBlockSize = 0x100;
constexpr std::uint32_t kStrmScratchSize = 0x10;

constexpr std::uint32_t kPngSignatureSize = 8;
constexpr std::uint32_t kHeaderSize = 8 + 12 + 13;  // signature + IHDR chunk
constexpr std::uint32_t kMaxFileNameLength = 1024;
constexpr std::uint32_t kMaxStreamCalls = 4096;
constexpr std::uint32_t kMaxDumpedImages = 64;

constexpr std::uint8_t kPngSignature[kPngSignatureSize] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

struct Handle {
  std::uint32_t malloc_func = 0;
  std::uint32_t malloc_arg = 0;
  std::uint32_t free_func = 0;
  std::uint32_t free_arg = 0;
};

enum class Phase { Opened, HeaderRead, ParamSet };

struct Stream {
  std::uint32_t handle = 0;
  std::vector<std::uint8_t> data;
  std::uint32_t strm_func = 0;  // cbCtrlStrm: 0 when the whole image is in `data`
  std::uint32_t strm_arg = 0;
  Phase phase = Phase::Opened;

  // IHDR.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  LodePNGColorType png_type = LCT_RGBA;
  std::uint32_t png_depth = 8;
  std::uint32_t interlace = 0;
  std::uint32_t in_space = 0;  // CellPngDecColorSpace of the stored image

  // SetParameter.
  std::uint32_t out_space = 0;
  std::uint32_t out_depth = 8;
  bool raw = false;  // output equals the stored format
  LodePNGColorType out_type = LCT_RGBA;
  bool fix_alpha = false;
  std::uint32_t alpha = 0;
  bool flip = false;
  std::uint64_t width_byte = 0;
  std::uint32_t out_counts = 0;
};

std::map<std::uint32_t, Handle> g_handles;
std::map<std::uint32_t, Stream> g_streams;
std::optional<std::filesystem::path> g_dump_dir;
std::uint32_t g_dump_count = 0;

bool mapped(std::uint32_t addr, std::uint32_t size) {
  return addr != 0 && process().memory.isCommitted(addr, size);
}

std::uint32_t componentCount(std::uint32_t space) {
  switch (space) {
    case kRgba:
    case kArgb: return 4;
    case kRgb: return 3;
    case kGrayscaleAlpha: return 2;
    default: return 1;
  }
}

bool spaceHasAlpha(std::uint32_t space) {
  return space == kRgba || space == kArgb || space == kGrayscaleAlpha;
}

// libpng colour type -> CellPngDecColorSpace; 0 for a type this decoder does not know.
std::uint32_t spaceOfType(LodePNGColorType type) {
  switch (type) {
    case LCT_GREY: return kGrayscale;
    case LCT_RGB: return kRgb;
    case LCT_PALETTE: return kPalette;
    case LCT_GREY_ALPHA: return kGrayscaleAlpha;
    case LCT_RGBA: return kRgba;
    default: return 0;
  }
}

std::uint32_t fnv1a(const std::vector<std::uint8_t>& data) {
  std::uint32_t hash = 2166136261u;
  for (const std::uint8_t byte : data) hash = (hash ^ byte) * 16777619u;
  return hash;
}

// Chunk bits of CellPngDecInfo::chunkInformation, in RPCS3's order.
struct ChunkScan {
  std::uint32_t info = 0;
  std::uint32_t text = 0;
  std::uint32_t unknown = 0;
  bool iend = false;  // the IEND chunk is complete
};

// Walks the chunks after the signature. `until_idat` stops at the first IDAT, which is what a
// reader that has only parsed the header has seen. Unknown chunks are counted when ancillary and
// safe to copy (the PNG_HANDLE_CHUNK_IF_SAFE policy RPCS3 sets).
ChunkScan scanChunks(const std::vector<std::uint8_t>& png, bool until_idat) {
  static constexpr std::pair<const char*, std::uint32_t> kBits[] = {
      {"gAMA", 0}, {"sBIT", 1}, {"cHRM", 2}, {"PLTE", 3}, {"tRNS", 4},  {"bKGD", 5},
      {"hIST", 6}, {"pHYs", 7}, {"oFFs", 8}, {"tIME", 9}, {"pCAL", 10}, {"sRGB", 11},
      {"iCCP", 12}, {"sPLT", 13}, {"sCAL", 14}};
  static constexpr const char* kKnownOther[] = {"IHDR", "IDAT", "IEND", "tEXt", "zTXt", "iTXt",
                                                "eXIf", "acTL", "fcTL", "fdAT"};
  ChunkScan scan;
  std::size_t pos = kPngSignatureSize;
  while (pos + 12 <= png.size()) {
    const std::uint8_t* chunk = png.data() + pos;
    const std::uint64_t length = (std::uint32_t{chunk[0]} << 24) | (std::uint32_t{chunk[1]} << 16) |
                                 (std::uint32_t{chunk[2]} << 8) | chunk[3];
    if (pos + 12 + length > png.size()) break;
    const std::string_view name(reinterpret_cast<const char*>(chunk + 4), 4);
    if (name == "IDAT" && until_idat) break;
    bool known = false;
    for (const auto& [chunk_name, bit] : kBits) {
      if (name == chunk_name) {
        scan.info |= 1u << bit;
        known = true;
      }
    }
    if (name == "tEXt" || name == "zTXt" || name == "iTXt") scan.text++;
    if (!known) {
      known = std::ranges::any_of(kKnownOther, [&](const char* other) { return name == other; });
      if (!known && (chunk[4] & 0x20) != 0 && (chunk[7] & 0x20) != 0) {
        scan.unknown++;
      }
    }
    if (name == "IEND") {
      scan.iend = true;
      break;
    }
    pos += 12 + length;
  }
  return scan;
}

// Calls a guest allocation callback; returns the callback's result.
std::uint32_t callMalloc(DalPpuContext* ctx, std::uint8_t* base, const Handle& handle,
                         std::uint32_t size) {
  return static_cast<std::uint32_t>(
      callGuestFunction(ctx, base, handle.malloc_func, {size, handle.malloc_arg}));
}

bool callFree(DalPpuContext* ctx, std::uint8_t* base, const Handle& handle, std::uint32_t addr) {
  return callGuestFunction(ctx, base, handle.free_func, {addr, handle.free_arg}) == 0;
}

Handle* findHandle(std::uint32_t addr) {
  const auto it = g_handles.find(addr);
  return it == g_handles.end() ? nullptr : &it->second;
}

Stream* findStream(std::uint32_t handle, std::uint32_t addr) {
  const auto it = g_streams.find(addr);
  return it == g_streams.end() || it->second.handle != handle ? nullptr : &it->second;
}

// Pulls data through cbCtrlStrm until `done` holds or the stream ends. Returns kCellOk or an
// error code.
template <typename Done>
std::uint32_t pullStream(DalPpuContext* ctx, std::uint8_t* base, const Handle& handle, Stream& stream,
                         Done done) {
  if (done()) return kCellOk;
  if (stream.strm_func == 0) return kErrorStreamFormat;
  const std::uint32_t scratch = callMalloc(ctx, base, handle, kStrmScratchSize);
  if (!mapped(scratch, kStrmScratchSize)) return kErrorFatal;
  std::uint32_t result = kCellOk;
  const std::size_t start_size = stream.data.size();
  std::uint32_t calls = 0;
  for (std::uint32_t call = 0; !done(); call++) {
    if (call >= kMaxStreamCalls) {
      logWarn("dal: pngdec: stream callback gave no end of image after {} calls", call);
      result = kErrorStreamFormat;
      break;
    }
    const std::uint32_t info = scratch, param = scratch + 8;
    dalWrite32(base, info, static_cast<std::uint32_t>(stream.data.size()));
    dalWrite32(base, param, 0);
    dalWrite32(base, param + 4, 0);
    const auto status = static_cast<std::uint32_t>(
        callGuestFunction(ctx, base, stream.strm_func, {info, param, stream.strm_arg}));
    const std::uint32_t ptr = dalRead32(base, param);
    const std::uint32_t size = dalRead32(base, param + 4);
    if (status != 0 || size == 0 || !mapped(ptr, size)) {
      logWarn("dal: pngdec: stream callback returned {:#x}, {} bytes at {:#010x}", status, size, ptr);
      result = kErrorStreamFormat;
      break;
    }
    stream.data.insert(stream.data.end(), base + ptr, base + ptr + size);
    calls++;
  }
  logInfo("dal: pngdec: stream callback: {} calls, {} -> {} bytes", calls, start_size,
          stream.data.size());
  if (!callFree(ctx, base, handle, scratch)) {
    logWarn("dal: pngdec: freeing the stream scratch block failed");
  }
  return result;
}

// Fills the IHDR fields of `stream` from its data.
std::uint32_t inspect(Stream& stream) {
  lodepng::State state;
  unsigned w = 0, h = 0;
  const unsigned error = lodepng_inspect(&w, &h, &state, stream.data.data(), stream.data.size());
  if (error != 0 || w == 0 || h == 0) {
    logWarn("dal: pngdec: bad PNG header: {}", lodepng_error_text(error));
    return kErrorHeader;
  }
  const LodePNGColorType type = state.info_png.color.colortype;
  stream.width = w;
  stream.height = h;
  stream.png_type = type;
  stream.png_depth = state.info_png.color.bitdepth;
  stream.interlace = state.info_png.interlace_method;
  stream.in_space = spaceOfType(type);
  return stream.in_space == 0 ? kErrorHeader : kCellOk;
}

std::optional<std::vector<std::uint8_t>> readFileRange(const std::filesystem::path& path,
                                                       std::int64_t offset, std::uint32_t size) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return std::nullopt;
  const auto file_size = static_cast<std::int64_t>(in.tellg());
  if (offset < 0 || offset > file_size) return std::nullopt;
  const std::int64_t available = file_size - offset;
  const std::int64_t count = size == 0 ? available : std::min<std::int64_t>(size, available);
  std::vector<std::uint8_t> data(static_cast<std::size_t>(count));
  in.seekg(offset);
  in.read(reinterpret_cast<char*>(data.data()), count);
  if (!in) return std::nullopt;
  return data;
}

// cellPngDecCreate(CellPngDecMainHandle* mainHandle, const CellPngDecThreadInParam* threadInParam,
//                  CellPngDecThreadOutParam* threadOutParam)
void pngCreate(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t handle_pp = argU32(ctx, 0);
  const std::uint32_t in_param = argU32(ctx, 1);
  const std::uint32_t out_param = argU32(ctx, 2);
  if (!mapped(handle_pp, 4) || !mapped(in_param, kThreadInSize) || !mapped(out_param, kThreadOutSize)) {
    setResultCode(ctx, kErrorArg);
    return;
  }
  Handle handle;
  handle.malloc_func = dalRead32(base, in_param + kThreadInMallocFunc);
  handle.malloc_arg = dalRead32(base, in_param + kThreadInMallocArg);
  handle.free_func = dalRead32(base, in_param + kThreadInFreeFunc);
  handle.free_arg = dalRead32(base, in_param + kThreadInFreeArg);
  if (handle.malloc_func == 0 || handle.free_func == 0) {
    setResultCode(ctx, kErrorArg);
    return;
  }
  const std::uint32_t block = callMalloc(ctx, base, handle, kHandleBlockSize);
  if (!mapped(block, kHandleBlockSize)) {
    logError("dal: pngdec: the handle allocation callback returned {:#010x}", block);
    setResultCode(ctx, kErrorFatal);
    return;
  }
  dalWrite32(base, block, handle.malloc_func);
  dalWrite32(base, block + 4, handle.malloc_arg);
  dalWrite32(base, block + 8, handle.free_func);
  dalWrite32(base, block + 12, handle.free_arg);
  g_handles[block] = handle;
  dalWrite32(base, handle_pp, block);
  dalWrite32(base, out_param, kCodecVersion);
  logInfo("dal: pngdec: create handle {:#010x}", block);
  setResultCode(ctx, kCellOk);
}

// cellPngDecDestroy(CellPngDecMainHandle mainHandle)
void pngDestroy(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t addr = argU32(ctx, 0);
  Handle* handle = findHandle(addr);
  if (handle == nullptr) {
    setResultCode(ctx, kErrorArg);
    return;
  }
  const Handle copy = *handle;
  g_handles.erase(addr);
  if (!callFree(ctx, base, copy, addr)) {
    logError("dal: pngdec: the handle free callback failed");
    setResultCode(ctx, kErrorFatal);
    return;
  }
  setResultCode(ctx, kCellOk);
}

// cellPngDecExtOpen(CellPngDecMainHandle mainHandle, CellPngDecSubHandle* subHandle,
//                   const CellPngDecSrc* src, CellPngDecOpnInfo* openInfo,
//                   const CellPngDecCbCtrlStrm* cbCtrlStrm, const CellPngDecOpnParam* opnParam)
void pngExtOpen(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t handle_addr = argU32(ctx, 0);
  const std::uint32_t stream_pp = argU32(ctx, 1);
  const std::uint32_t src = argU32(ctx, 2);
  const std::uint32_t open_info = argU32(ctx, 3);
  const std::uint32_t cb_strm = argU32(ctx, 4);
  const std::uint32_t opn_param = argU32(ctx, 5);
  Handle* handle = findHandle(handle_addr);
  if (handle == nullptr || !mapped(stream_pp, 4) || !mapped(src, kSrcSize) || !mapped(open_info, 4) ||
      (cb_strm != 0 && !mapped(cb_strm, kCbCtrlStrmSize))) {
    setResultCode(ctx, kErrorArg);
    return;
  }

  Stream stream;
  stream.handle = handle_addr;
  if (cb_strm != 0) {
    stream.strm_func = dalRead32(base, cb_strm);
    stream.strm_arg = dalRead32(base, cb_strm + 4);
  }
  if (opn_param != 0 && mapped(opn_param, 4) && dalRead32(base, opn_param) != 0) {
    logOnce("pngdec-selectchunk", "dal: pngdec: opnParam.selectChunk {:#x} is ignored",
            dalRead32(base, opn_param));
  }

  const std::uint32_t select = dalRead32(base, src);
  std::string origin;
  if (select == kSrcFile) {
    if (stream.strm_func != 0) {
      logError("dal: pngdec: partial decoding from a file is not possible");
      setResultCode(ctx, kErrorStreamFormat);
      return;
    }
    const std::uint32_t name_addr = dalRead32(base, src + kSrcFileName);
    if (!mapped(name_addr, 1)) {
      setResultCode(ctx, kErrorArg);
      return;
    }
    const std::string name = readGuestString(base, name_addr, kMaxFileNameLength);
    const auto offset = static_cast<std::int64_t>(dalRead64(base, src + kSrcFileOffset));
    const std::uint32_t size = dalRead32(base, src + kSrcFileSize);
    bool read_only = false;
    const auto host = resolveGuestPath(name, read_only);
    auto data = host ? readFileRange(*host, offset, size) : std::nullopt;
    if (!data) {
      logError("dal: pngdec: cannot open {}", name);
      setResultCode(ctx, kErrorOpenFile);
      return;
    }
    stream.data = std::move(*data);
    origin = std::format("file {} offset {} size {}", name, offset, stream.data.size());
  } else if (select == kSrcBuffer) {
    const std::uint32_t ptr = dalRead32(base, src + kSrcStreamPtr);
    const std::uint32_t size = dalRead32(base, src + kSrcStreamSize);
    if (size == 0 || !mapped(ptr, size)) {
      setResultCode(ctx, kErrorArg);
      return;
    }
    stream.data.assign(base + ptr, base + ptr + size);
    origin = std::format("buffer {:#010x} size {}{}", ptr, size,
                         stream.strm_func != 0 ? ", streamed" : "");
  } else {
    setResultCode(ctx, kErrorArg);
    return;
  }

  if (stream.data.size() < kPngSignatureSize) {
    setResultCode(ctx, kErrorHeader);
    return;
  }
  if (std::memcmp(stream.data.data(), kPngSignature, kPngSignatureSize) != 0) {
    logError("dal: pngdec: not a PNG signature ({})", origin);
    setResultCode(ctx, kErrorHeader);
    return;
  }

  const std::uint32_t block = callMalloc(ctx, base, *handle, kStreamBlockSize);
  if (!mapped(block, kStreamBlockSize)) {
    logError("dal: pngdec: the stream allocation callback returned {:#010x}", block);
    setResultCode(ctx, kErrorFatal);
    return;
  }
  logInfo("dal: pngdec: open stream {:#010x} from {} hash {:08x}", block, origin, fnv1a(stream.data));
  g_streams[block] = std::move(stream);
  dalWrite32(base, stream_pp, block);
  dalWrite32(base, open_info, kStreamBlockSize);
  setResultCode(ctx, kCellOk);
}

// cellPngDecClose(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle)
void pngClose(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t handle_addr = argU32(ctx, 0);
  const std::uint32_t stream_addr = argU32(ctx, 1);
  Handle* handle = findHandle(handle_addr);
  if (handle == nullptr || findStream(handle_addr, stream_addr) == nullptr) {
    setResultCode(ctx, kErrorArg);
    return;
  }
  g_streams.erase(stream_addr);
  if (!callFree(ctx, base, *handle, stream_addr)) {
    logError("dal: pngdec: the stream free callback failed");
    setResultCode(ctx, kErrorFatal);
    return;
  }
  setResultCode(ctx, kCellOk);
}

// cellPngDecExtReadHeader(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle,
//                         CellPngDecInfo* info, CellPngDecExtInfo* extInfo)
void pngExtReadHeader(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t handle_addr = argU32(ctx, 0);
  const std::uint32_t stream_addr = argU32(ctx, 1);
  const std::uint32_t info = argU32(ctx, 2);
  const std::uint32_t ext_info = argU32(ctx, 3);
  Handle* handle = findHandle(handle_addr);
  Stream* stream = handle != nullptr ? findStream(handle_addr, stream_addr) : nullptr;
  if (stream == nullptr || !mapped(info, kInfoSize) || (ext_info != 0 && !mapped(ext_info, 8))) {
    setResultCode(ctx, kErrorArg);
    return;
  }
  if (ext_info != 0) dalWrite64(base, ext_info, 0);

  std::uint32_t result = pullStream(ctx, base, *handle, *stream,
                                    [&] { return stream->data.size() >= kHeaderSize; });
  if (result == kCellOk) result = inspect(*stream);
  if (result != kCellOk) {
    setResultCode(ctx, result == kErrorStreamFormat ? kErrorHeader : result);
    return;
  }
  const ChunkScan scan = scanChunks(stream->data, true);
  const std::uint32_t components = componentCount(stream->in_space);
  dalWrite32(base, info, stream->width);
  dalWrite32(base, info + 4, stream->height);
  dalWrite32(base, info + 8, components);
  dalWrite32(base, info + 12, stream->in_space);
  dalWrite32(base, info + 16, stream->png_depth);
  dalWrite32(base, info + 20, stream->interlace);
  dalWrite32(base, info + 24, scan.info);
  if (stream->phase == Phase::Opened) stream->phase = Phase::HeaderRead;
  logInfo("dal: pngdec: stream {:#010x}: {}x{} colorspace {} depth {} interlace {} chunks {:#06x}",
          stream_addr, stream->width, stream->height, stream->in_space, stream->png_depth,
          stream->interlace, scan.info);
  setResultCode(ctx, kCellOk);
}

// cellPngDecExtSetParameter(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle,
//                           const CellPngDecInParam* inParam, CellPngDecOutParam* outParam,
//                           const CellPngDecExtInParam* extInParam,
//                           CellPngDecExtOutParam* extOutParam)
void pngExtSetParameter(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t handle_addr = argU32(ctx, 0);
  const std::uint32_t stream_addr = argU32(ctx, 1);
  const std::uint32_t in_param = argU32(ctx, 2);
  const std::uint32_t out_param = argU32(ctx, 3);
  const std::uint32_t ext_in = argU32(ctx, 4);
  const std::uint32_t ext_out = argU32(ctx, 5);
  Stream* stream = findHandle(handle_addr) != nullptr ? findStream(handle_addr, stream_addr) : nullptr;
  if (stream == nullptr || !mapped(in_param, kInParamSize) || !mapped(out_param, kOutParamSize) ||
      (ext_in != 0 && !mapped(ext_in, kExtInParamSize)) ||
      (ext_out != 0 && !mapped(ext_out, kExtOutParamSize))) {
    setResultCode(ctx, kErrorArg);
    return;
  }
  if (stream->phase == Phase::Opened) {
    setResultCode(ctx, kErrorSeq);
    return;
  }

  const std::uint32_t command_ptr = dalRead32(base, in_param);
  const std::uint32_t output_mode = dalRead32(base, in_param + 4);
  const std::uint32_t out_space = dalRead32(base, in_param + 8);
  const std::uint32_t out_depth = dalRead32(base, in_param + 12);
  const std::uint32_t pack = dalRead32(base, in_param + 16);
  const std::uint32_t alpha_select = dalRead32(base, in_param + 20);
  const std::uint32_t color_alpha = dalRead32(base, in_param + 24);
  if (command_ptr != 0) logOnce("pngdec-command", "dal: pngdec: commandPtr is ignored");
  if (pack != kPackOneBytePerPixel) {
    logOnce("pngdec-pack", "dal: pngdec: outputPackFlag {} is treated as 1 byte per pixel", pack);
  }

  const bool space_ok = out_space == kGrayscale || out_space == kRgb || out_space == kPalette ||
                        out_space == kGrayscaleAlpha || out_space == kRgba || out_space == kArgb;
  if (!space_ok || output_mode > 1 || alpha_select > 1) {
    logError("dal: pngdec: bad output parameters: colorspace {} mode {} alpha select {}", out_space,
             output_mode, alpha_select);
    setResultCode(ctx, kErrorArg);
    return;
  }

  Stream update = *stream;
  update.out_space = out_space;
  update.out_depth = out_depth;
  update.raw = out_space == update.in_space && out_depth == update.png_depth;
  if (!update.raw) {
    if ((out_depth != 8 && out_depth != 16) || out_space == kPalette) {
      logError("dal: pngdec: cannot convert colorspace {} depth {} to colorspace {} depth {}",
               update.in_space, update.png_depth, out_space, out_depth);
      setResultCode(ctx, kErrorArg);
      return;
    }
    // lodepng cannot reduce colour to grayscale; RPCS3 does it through libpng's
    // png_set_rgb_to_gray.
    const bool in_color = update.in_space == kRgb || update.in_space == kRgba;
    if ((out_space == kGrayscale || out_space == kGrayscaleAlpha) && in_color) {
      logError("dal: pngdec: RGB to grayscale conversion is not implemented");
      setResultCode(ctx, kErrorArg);
      return;
    }
    switch (out_space) {
      case kGrayscale: update.out_type = LCT_GREY; break;
      case kRgb: update.out_type = LCT_RGB; break;
      case kGrayscaleAlpha: update.out_type = LCT_GREY_ALPHA; break;
      default: update.out_type = LCT_RGBA; break;
    }
  }
  update.fix_alpha = !update.raw && alpha_select == kFixAlpha && spaceHasAlpha(out_space) &&
                     !spaceHasAlpha(update.in_space);
  update.alpha = color_alpha;
  update.flip = output_mode == kBottomToTop;
  const std::uint32_t components = componentCount(out_space);
  update.width_byte = update.raw && out_depth < 8
                          ? (std::uint64_t{update.width} * out_depth + 7) / 8
                          : std::uint64_t{update.width} * components * (out_depth / 8);

  update.out_counts = 0;
  if (ext_in != 0) {
    if (dalRead32(base, ext_in) != kLineMode) {
      logError("dal: pngdec: bufferMode {} is not CELL_PNGDEC_LINE_MODE", dalRead32(base, ext_in));
      setResultCode(ctx, kErrorArg);
      return;
    }
    // Adam7 images come out one scan pass at a time in RPCS3; the count does not apply.
    update.out_counts = update.interlace != 0 ? 1 : dalRead32(base, ext_in + 4);
  }
  if (ext_out != 0) {
    dalWrite64(base, ext_out, update.width_byte);
    dalWrite32(base, ext_out + 8,
               update.out_counts == 0 ? update.height : std::min(update.out_counts, update.height));
  }

  dalWrite64(base, out_param, update.width_byte);
  dalWrite32(base, out_param + 8, update.width);
  dalWrite32(base, out_param + 12, update.height);
  dalWrite32(base, out_param + 16, components);
  dalWrite32(base, out_param + 20, out_depth);
  dalWrite32(base, out_param + 24, output_mode);
  dalWrite32(base, out_param + 28, out_space);
  dalWrite32(base, out_param + 32, 0);  // useMemorySpace: nothing is taken from the game's heap
  update.phase = Phase::ParamSet;
  *stream = std::move(update);
  logInfo("dal: pngdec: stream {:#010x}: output colorspace {} depth {} {} bytes per row{}{}",
          stream_addr, out_space, out_depth, stream->width_byte,
          stream->flip ? ", bottom to top" : "", stream->fix_alpha ? ", fixed alpha" : "");
  setResultCode(ctx, kCellOk);
}

// Writes the decoded image as 8-bit RGBA PNG for inspection.
void dumpImage(const Stream& stream) {
  if (!g_dump_dir || g_dump_count >= kMaxDumpedImages) return;
  unsigned char* pixels = nullptr;
  unsigned w = 0, h = 0;
  if (lodepng_decode32(&pixels, &w, &h, stream.data.data(), stream.data.size()) != 0) return;
  const std::filesystem::path path = *g_dump_dir / std::format("png_{}.png", g_dump_count++);
  const unsigned error = lodepng_encode32_file(path.string().c_str(), pixels, w, h);
  std::free(pixels);
  if (error != 0) {
    logWarn("dal: pngdec: cannot write {}: {}", path.string(), lodepng_error_text(error));
  } else {
    logInfo("dal: pngdec: dumped {}x{} image to {}", w, h, path.string());
  }
}

// Decodes the stream into the output format chosen by SetParameter. Returns the pixel rows
// (width_byte bytes each, top to bottom) or an error code.
std::optional<std::vector<std::uint8_t>> decodePixels(const Stream& stream) {
  lodepng::State state;
  state.decoder.color_convert = stream.raw ? 0 : 1;
  state.decoder.read_text_chunks = 0;
  if (!stream.raw) {
    state.info_raw.colortype = stream.out_type;
    state.info_raw.bitdepth = stream.out_depth;
  }
  unsigned char* out = nullptr;
  unsigned w = 0, h = 0;
  const unsigned error = lodepng_decode(&out, &w, &h, &state, stream.data.data(), stream.data.size());
  if (error != 0) {
    logError("dal: pngdec: decode failed: {}", lodepng_error_text(error));
    return std::nullopt;
  }
  const std::size_t size =
      lodepng_get_raw_size(w, h, stream.raw ? &state.info_png.color : &state.info_raw);
  std::vector<std::uint8_t> pixels(out, out + size);
  std::free(out);
  if (w != stream.width || h != stream.height || pixels.size() != stream.width_byte * h) {
    logError("dal: pngdec: decoded image is {}x{} with {} bytes, expected {}x{} with {}", w, h,
             pixels.size(), stream.width, stream.height, stream.width_byte * stream.height);
    return std::nullopt;
  }

  const std::size_t sample = stream.out_depth / 8;
  const std::size_t components = componentCount(stream.out_space == kArgb ? kRgba : stream.out_space);
  if (stream.fix_alpha) {
    // RGBA or gray-alpha, alpha last (ARGB is swizzled below).
    const std::size_t pixel_size = components * sample;
    for (std::size_t pos = 0; pos + pixel_size <= pixels.size(); pos += pixel_size) {
      std::uint8_t* alpha = pixels.data() + pos + (components - 1) * sample;
      if (sample == 1) {
        alpha[0] = static_cast<std::uint8_t>(stream.alpha);
      } else {
        alpha[0] = static_cast<std::uint8_t>(stream.alpha >> 8);
        alpha[1] = static_cast<std::uint8_t>(stream.alpha);
      }
    }
  }
  if (stream.out_space == kArgb) {
    const std::size_t pixel_size = 4 * sample;
    for (std::size_t pos = 0; pos + pixel_size <= pixels.size(); pos += pixel_size) {
      std::rotate(pixels.begin() + pos, pixels.begin() + pos + 3 * sample,
                  pixels.begin() + pos + pixel_size);
    }
  }
  return pixels;
}

// cellPngDecExtDecodeData(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle,
//                         uint8_t* data, const CellPngDecDataCtrlParam* dataCtrlParam,
//                         CellPngDecDataOutInfo* dataOutInfo, const CellPngDecCbCtrlDisp* cbCtrlDisp,
//                         CellPngDecDispParam* dispParam)
void pngExtDecodeData(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t handle_addr = argU32(ctx, 0);
  const std::uint32_t stream_addr = argU32(ctx, 1);
  const std::uint32_t data = argU32(ctx, 2);
  const std::uint32_t ctrl_param = argU32(ctx, 3);
  const std::uint32_t out_info = argU32(ctx, 4);
  const std::uint32_t cb_disp = argU32(ctx, 5);
  Handle* handle = findHandle(handle_addr);
  Stream* stream = handle != nullptr ? findStream(handle_addr, stream_addr) : nullptr;
  if (stream == nullptr || !mapped(ctrl_param, 8) || !mapped(out_info, kDataOutInfoSize)) {
    setResultCode(ctx, kErrorArg);
    return;
  }
  dalWrite32(base, out_info + 12, kStatusStop);  // stays STOP if an error ends the call
  if (stream->phase != Phase::ParamSet) {
    setResultCode(ctx, kErrorSeq);
    return;
  }
  if (cb_disp != 0 && stream->out_counts > 0) {
    logError("dal: pngdec: partial output through cbCtrlDisp is not implemented");
    setResultCode(ctx, kErrorArg);
    return;
  }
  const std::uint64_t bytes_per_line = dalRead64(base, ctrl_param);
  const std::uint64_t width_byte = stream->width_byte;
  const std::uint64_t span = width_byte + (stream->height - 1) * bytes_per_line;
  if (bytes_per_line < width_byte || bytes_per_line > 0xffffffffull || span > 0xffffffffull ||
      !mapped(data, static_cast<std::uint32_t>(span))) {
    logError("dal: pngdec: bad output buffer {:#010x}, {} bytes per line (row needs {})", data,
             bytes_per_line, width_byte);
    setResultCode(ctx, kErrorArg);
    return;
  }

  const std::uint32_t pulled = pullStream(ctx, base, *handle, *stream,
                                          [&] { return scanChunks(stream->data, false).iend; });
  if (pulled != kCellOk && stream->strm_func != 0) {
    setResultCode(ctx, pulled);
    return;
  }
  const auto pixels = decodePixels(*stream);
  if (!pixels) {
    setResultCode(ctx, kErrorStreamFormat);
    return;
  }
  for (std::uint32_t row = 0; row < stream->height; row++) {
    const std::uint32_t line = stream->flip ? stream->height - 1 - row : row;
    std::memcpy(base + data + line * bytes_per_line, pixels->data() + row * width_byte, width_byte);
  }

  const ChunkScan scan = scanChunks(stream->data, false);
  dalWrite32(base, out_info, scan.info | (1u << 15));  // IDAT
  dalWrite32(base, out_info + 4, scan.text);
  dalWrite32(base, out_info + 8, scan.unknown);
  dalWrite32(base, out_info + 12, kStatusFinish);
  logInfo("dal: pngdec: stream {:#010x}: decoded {}x{} to {:#010x}, {} bytes per line", stream_addr,
          stream->width, stream->height, data, bytes_per_line);
  dumpImage(*stream);
  setResultCode(ctx, kCellOk);
}

constexpr HleEntry kHandlers[] = {
    {kModule, 0x0c515302, "cellPngDecExtOpen", pngExtOpen},
    {kModule, 0x157d30c5, "cellPngDecCreate", pngCreate},
    {kModule, 0x5b3d1ff1, "cellPngDecClose", pngClose},
    {kModule, 0x726fc1d0, "cellPngDecExtDecodeData", pngExtDecodeData},
    {kModule, 0x820dae1a, "cellPngDecDestroy", pngDestroy},
    {kModule, 0x8b33f863, "cellPngDecExtReadHeader", pngExtReadHeader},
    {kModule, 0x9e9d7d42, "cellPngDecExtSetParameter", pngExtSetParameter},
};

}  // namespace

std::span<const HleEntry> cellPngDecHandlers() { return kHandlers; }

void setPngDumpDirectory(std::optional<std::filesystem::path> dir) { g_dump_dir = std::move(dir); }

void resetCellPngDec() {
  g_handles.clear();
  g_streams.clear();
  g_dump_dir.reset();
  g_dump_count = 0;
}

}  // namespace dal
