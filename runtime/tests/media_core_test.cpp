// Self-checking tests for the movie pipeline's core: cellSail guest layouts, codec interfaces and the
// service thread, and the PAMF parser and demuxer. No FFmpeg is involved. Only the PAMF game-file
// test reads game data; it is optional and runs only when DAL_GAME_DIR is set.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/boot.h"
#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/host_functions.h"
#include "core/log.h"
#include "core/threads.h"
#include "dal/ppu_context.h"
#include "hle/cell_sail_types.h"
#include "hle/hle.h"
#include "hle/sail_service.h"
#include "lv2/sys_sync.h"
#include "media/movie_decoder.h"
#include "media/movie_player.h"
#include "media/pamf.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (cond) {                                                              \
      g_passed++;                                                            \
    } else {                                                                 \
      g_failed++;                                                            \
      std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
    }                                                                        \
  } while (0)

void testGuestLayouts() {
  namespace sail = dal::sail;

  // Sizes and offsets of the structs the HLE reads and writes.
  CHECK(sail::kVideoFormatSize == 32 && sail::kVideoFormatWidth == 4 && sail::kVideoFormatHeight == 6 &&
        sail::kVideoFormatPitch == 8 && sail::kVideoFormatAlpha == 12 &&
        sail::kVideoFormatColorMatrix == 16 && sail::kVideoFormatReserved3 == 24);
  CHECK(sail::kAudioFormatSize == 24 && sail::kAudioFormatSampleNum == 2 && sail::kAudioFormatFs == 4 &&
        sail::kAudioFormatChLayout == 8 && sail::kAudioFormatReserved1 == 16);
  CHECK(sail::kFrameInfoSize == 24 && sail::kFrameInfoTag == 8 && sail::kFrameInfoStatus == 12 &&
        sail::kFrameInfoPts == 16);
  CHECK(sail::kAllocatorFuncsSize == 8 && sail::kAllocatorSize == 8 && sail::kAllocatorArg == 4);
  CHECK(sail::kSoundAdapterFuncsSize == 12 && sail::kGraphicsAdapterFuncsSize == 20 &&
        sail::kGraphicsAdapterFuncsAlloc == 12 && sail::kGraphicsAdapterFuncsFree == 16);
  CHECK(sail::kPlayerAttributeSize == 32 && sail::kPlayerResourceSize == 16 &&
        sail::kStartCommandSize == 40 && sail::kEventSize == 8);
  CHECK(sail::kPamfReaderSize == 128);

  // The event value is (major << 32) | minor.
  CHECK(sail::makeEvent(sail::kEventPlayerCallCompleted, sail::kCallStart) == 0x000000020000000aull);
  CHECK(sail::makeEvent(sail::kEventPlayerStateChanged, 0) == 0x0000000300000000ull);

  // Event majors, states and call ids the game's callback and the HLE agree on.
  CHECK(sail::kEventErrorOccured == 1 && sail::kEventPlayerCallCompleted == 2 &&
        sail::kEventPlayerStateChanged == 3 && sail::kEventMediaStateChanged == 11);
  CHECK(static_cast<std::uint32_t>(sail::PlayerState::Running) == 6 &&
        static_cast<std::uint32_t>(sail::PlayerState::Lost) == 9);
  CHECK(sail::kCallBoot == 1 && sail::kCallOpenStream == 2 && sail::kCallStart == 10 &&
        sail::kCallReopenEsUser == 15);

  // The game consumes a frame only for a status word >= 0x100.
  CHECK(sail::kSyncFrameConsumedMin == 0x100 && sail::kSyncNextOnTime == 0x204);
  CHECK(sail::kSyncNoFrame < sail::kSyncFrameConsumedMin);

  CHECK(sail::kVideoCodingArgbInterleaved == 0 && sail::kVideoCodingYuv420Planar == 3);
  CHECK(sail::kVideoColorRangeFull == 0 && sail::kVideoColorRangeLimited == 1);
  CHECK(sail::kAudioCodingLpcmFloat32 == 1 && sail::kAudioChLayout2ChLr == 2);

  CHECK(sail::kErrorInvalidArg == 0x80610701 && sail::kErrorInvalidState == 0x80610702 &&
        sail::kErrorFatal == 0x806107FF);
  CHECK(sail::kPamfErrorStreamNotFound == 0x80610501 && sail::kPamfErrorNotAvailable == 0x80610508);
  CHECK(sail::kPamfStreamTypeVideo == 20 && sail::kPamfStreamTypeAudio == 21);
  CHECK(static_cast<int>(dal::PamfCodec::Avc) == sail::kPamfStreamTypeAvc &&
        static_cast<int>(dal::PamfCodec::Atrac3plus) == sail::kPamfStreamTypeAtrac3plus &&
        static_cast<int>(dal::PamfCodec::UserData) == sail::kPamfStreamTypeUserData);
}

class CountingFactory : public dal::MediaCodecFactory {
 public:
  explicit CountingFactory(int* destroyed) : destroyed_(destroyed) {}
  ~CountingFactory() override { ++*destroyed_; }
  std::unique_ptr<dal::VideoDecoder> createVideo(const dal::PamfStream&, const dal::VideoOutputFormat&,
                                                 std::string& error) override {
    error = "no video";
    return nullptr;
  }
  std::unique_ptr<dal::AudioDecoder> createAudio(const dal::PamfStream&, const dal::AudioOutputFormat&,
                                                 std::string& error) override {
    error = "no audio";
    return nullptr;
  }

 private:
  int* destroyed_;
};

void testCodecFactory() {
  CHECK(dal::mediaCodecFactory() == nullptr);

  int destroyed_a = 0, destroyed_b = 0;
  auto a = std::make_unique<CountingFactory>(&destroyed_a);
  dal::MediaCodecFactory* raw_a = a.get();
  dal::setMediaCodecFactory(std::move(a));
  CHECK(dal::mediaCodecFactory() == raw_a);

  std::string error;
  CHECK(dal::mediaCodecFactory()->createVideo({}, {}, error) == nullptr && error == "no video");
  CHECK(dal::mediaCodecFactory()->createAudio({}, {}, error) == nullptr && error == "no audio");

  // Replacing destroys the previous factory.
  auto b = std::make_unique<CountingFactory>(&destroyed_b);
  dal::MediaCodecFactory* raw_b = b.get();
  dal::setMediaCodecFactory(std::move(b));
  CHECK(dal::mediaCodecFactory() == raw_b && destroyed_a == 1 && destroyed_b == 0);

  dal::setMediaCodecFactory(nullptr);
  CHECK(dal::mediaCodecFactory() == nullptr && destroyed_b == 1);
}

struct ServiceCall {
  int id;
  bool on_service_thread;  // name, service flag and a TLS pointer of its own
  std::uint64_t guest_result;
  std::uint32_t thread_id;
};
std::vector<ServiceCall> g_calls;  // written under the guest lock
std::condition_variable_any g_calls_cv;

DAL_PPU_FUNC(sailFakeGuestFunction) {
  (void)base;
  ctx->gpr[3] = ctx->gpr[3] * 2;
}

template <class Pred>
bool waitUntil(Pred done) {
  return dal::blockingWait(g_calls_cv, done, "test wait", std::chrono::seconds(5));
}

void pauseUnlocked() {
  std::condition_variable_any cv;
  dal::blockingWait(cv, [] { return false; }, "test pause", std::chrono::milliseconds(2));
}

dal::SailCall recordingCall(int id, std::uint32_t guest_opd, std::function<void()> then = {}) {
  return [id, guest_opd, then = std::move(then)](DalPpuContext* ctx, std::uint8_t* base) {
    const dal::GuestThread* thread = dal::currentThread();
    const bool on_service = thread != nullptr && thread->service() && thread->name() == "_libsail-control" &&
                            ctx->gpr[13] == thread->tlsR13() && ctx->gpr[13] != 0;
    // The service thread can call guest functions through an OPD, as the cellSail callbacks do.
    const std::uint64_t result = dal::callGuestFunction(ctx, base, guest_opd, {static_cast<std::uint64_t>(id)});
    g_calls.push_back({id, on_service, result, thread != nullptr ? thread->id() : 0});
    if (then) then();
    g_calls_cv.notify_all();
  };
}

void testSailService() {
  dal::setLogSink([](std::string_view) {});
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetSyncObjects();
  dal::resetHostFunctions();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t tls_image = scratch + 0x100;
  g_calls.clear();

  std::lock_guard<dal::GuestLock> guard(dal::guestLock());
  auto boot_ctx = std::make_unique<DalPpuContext>();
  dalWrite32(base, tls_image, 0xCAFEF00D);
  CHECK(dal::initializeTls(boot_ctx.get(), base, 0x01000000, tls_image, 4, 0x20) != 0);
  const std::uint32_t guest_opd = dal::registerHostFunction("sailFakeGuestFunction", sailFakeGuestFunction);
  CHECK(guest_opd != 0);

  // The thread cannot run while this test holds the guest lock, so all of these queue up. The first
  // call queues one more from the service thread itself, which runs after the three already queued.
  dal::queueSailCall(recordingCall(1, guest_opd, [guest_opd] {
    dal::queueSailCall(recordingCall(4, guest_opd));
  }));
  dal::queueSailCall(recordingCall(2, guest_opd));
  dal::queueSailCall(recordingCall(3, guest_opd));
  CHECK(g_calls.empty());
  CHECK(waitUntil([] { return g_calls.size() >= 4; }));
  CHECK(g_calls.size() == 4);
  if (g_calls.size() == 4) {
    for (int i = 0; i < 4; i++) {
      CHECK(g_calls[i].id == i + 1);
      CHECK(g_calls[i].on_service_thread);
      CHECK(g_calls[i].guest_result == static_cast<std::uint64_t>(2 * (i + 1)));
    }
  }

  // Idle, the thread is a service thread: the census says so and the supervisor does not count it.
  const dal::GuestThread* thread = g_calls.empty() ? nullptr : dal::findThread(g_calls[0].thread_id);
  CHECK(thread != nullptr);
  if (thread == nullptr) return;
  for (int i = 0; i < 2500 && thread->state() != dal::ThreadState::Blocked; i++) pauseUnlocked();
  CHECK(thread->state() == dal::ThreadState::Blocked && thread->service());
  CHECK(dal::formatCensus().find("_libsail-control: blocked in sail control wait [service]") !=
        std::string::npos);
  const dal::ThreadHealth service_health{thread->state(), thread->indefiniteWait(), thread->blockedSinceMs(),
                                         thread->service()};
  const std::int64_t later = service_health.blocked_since_ms + 10000;
  CHECK(dal::judgeThreads({service_health}, later) == dal::ThreadVerdict::NoThreads);
  const dal::ThreadHealth running_game{dal::ThreadState::Running, false, 0, false};
  CHECK(dal::judgeThreads({service_health, running_game}, later) == dal::ThreadVerdict::Ok);

  // stopSailService drops calls that have not run and joins the thread.
  const std::size_t before_stop = g_calls.size();
  dal::queueSailCall(recordingCall(5, guest_opd));
  dal::queueSailCall(recordingCall(6, guest_opd));
  dal::stopSailService();
  CHECK(thread->state() == dal::ThreadState::Exited);
  CHECK(g_calls.size() == before_stop);

  // A later call starts a new thread.
  dal::queueSailCall(recordingCall(7, guest_opd));
  CHECK(waitUntil([&] { return g_calls.size() > before_stop; }));
  CHECK(g_calls.size() == before_stop + 1 && g_calls.back().id == 7 && g_calls.back().on_service_thread);
  dal::stopSailService();

  dal::resetHostFunctions();
  dal::resetSyncObjects();
  p.main.free(scratch);
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& v, std::size_t off, std::uint32_t x) {
  v[off] = static_cast<std::uint8_t>(x >> 8);
  v[off + 1] = static_cast<std::uint8_t>(x);
}
void put32(Bytes& v, std::size_t off, std::uint32_t x) {
  put16(v, off, x >> 16);
  put16(v, off + 2, x & 0xFFFF);
}

// A two-stream header laid out like the game's movie headers: AVC 1280x720 on 0xE0 and
// ATRAC3plus 2 channels 48 kHz on private stream 1 / sub-stream 0.
Bytes makePamfHeader(std::uint32_t data_units) {
  Bytes h(2048, 0);
  std::memcpy(h.data(), "PAMF0041", 8);
  put32(h, 0x08, 1);
  put32(h, 0x0C, data_units);
  put32(h, 0x50, 0x94);
  put32(h, 0x58, 89310);
  put32(h, 0x5E, 722910);
  put32(h, 0x62, 120000);
  put32(h, 0x66, 67500);
  put32(h, 0x6A, 2);
  h[0x6F] = 1;
  put32(h, 0x70, 0x74);
  put32(h, 0x76, 89310);
  put32(h, 0x7C, 722910);
  h[0x81] = 1;
  put32(h, 0x82, 0x62);
  h[0x87] = 2;
  const std::uint8_t video[] = {0x1B, 0, 0, 0, 0xE0, 0, 0x33, 0x3B};
  std::memcpy(&h[0x88], video, sizeof(video));
  h[0x88 + 0x10 + 0x09] = 0x50;
  h[0x88 + 0x10 + 0x0B] = 0x2D;
  const std::uint8_t audio[] = {0xDC, 0, 0, 0, 0xBD, 0x00, 0x20, 0x14};
  std::memcpy(&h[0xB8], audio, sizeof(audio));
  h[0xB8 + 0x10 + 0x02] = 2;
  h[0xB8 + 0x10 + 0x03] = 1;
  return h;
}

void testPamfHeader() {
  const Bytes h = makePamfHeader(7);
  auto info = dal::parsePamfHeader(h);
  CHECK(info.has_value());
  if (!info) return;
  CHECK(info->header_size == 2048 && info->data_offset == 2048 && info->data_size == 7 * 2048);
  CHECK(info->start_pts == 89310 && info->end_pts == 722910 && info->mux_rate_bound == 120000);
  CHECK(info->streams.size() == 2);
  if (info->streams.size() != 2) return;
  const dal::PamfStream& v = info->streams[0];
  CHECK(v.codec == dal::PamfCodec::Avc && v.stream_id == 0xE0 && v.channel == 0 && v.width == 1280 &&
        v.height == 720 && v.codec_info.size() == 32);
  const dal::PamfStream& a = info->streams[1];
  CHECK(a.codec == dal::PamfCodec::Atrac3plus && a.stream_id == 0xBD && a.private_stream_id == 0 &&
        a.channels == 2 && a.sample_rate == 48000);

  // Only the sequence information is needed, not the whole 2048-byte header.
  CHECK(dal::parsePamfHeader(std::span<const std::uint8_t>(h).first(0xE8)).has_value());

  auto fails = [&](auto mutate) {
    Bytes bad = h;
    mutate(bad);
    return !dal::parsePamfHeader(bad).has_value();
  };
  CHECK(!dal::parsePamfHeader(std::span<const std::uint8_t>(h).first(0xE0)).has_value());
  CHECK(!dal::parsePamfHeader({}).has_value());
  CHECK(fails([](Bytes& b) { b[0] = 'X'; }));
  CHECK(fails([](Bytes& b) { b[7] = '9'; }));
  CHECK(fails([](Bytes& b) { put32(b, 0x08, 0); }));
  CHECK(fails([](Bytes& b) { put32(b, 0x50, 0xFFFFFF); }));
  CHECK(fails([](Bytes& b) { put32(b, 0x6A, 3); }));
  CHECK(fails([](Bytes& b) { put32(b, 0x5E, 80000); }));
  CHECK(fails([](Bytes& b) { b[0xB8] = 0x55; }));
  CHECK(fails([](Bytes& b) { b[0xB8 + 5] = 0x40; }));       // LPCM sub-stream on an ATRAC3plus entry
  CHECK(fails([](Bytes& b) { b[0xB8] = 0x1B; b[0xB8 + 4] = 0xE0; }));  // second AVC stream on 0xE0
  CHECK(fails([](Bytes& b) { b[0xB8] = 0x02; b[0xB8 + 4] = 0xE0; }));  // MPEG-2 video on the same stream id
  CHECK(fails([](Bytes& b) { put32(b, 0x76, 89311); }));  // grouping period start differs from the start PTS
  CHECK(fails([](Bytes& b) { put32(b, 0x70, 0x75); }));   // period does not fit in the sequence info
  CHECK(fails([](Bytes& b) { put32(b, 0x82, 0x63); }));   // group does not fit in the period
}

void appendTimestamp(Bytes& v, int prefix, std::int64_t ts) {
  v.push_back(static_cast<std::uint8_t>((prefix << 4) | (((ts >> 30) & 7) << 1) | 1));
  v.push_back(static_cast<std::uint8_t>(ts >> 22));
  v.push_back(static_cast<std::uint8_t>((((ts >> 15) & 0x7F) << 1) | 1));
  v.push_back(static_cast<std::uint8_t>(ts >> 7));
  v.push_back(static_cast<std::uint8_t>(((ts & 0x7F) << 1) | 1));
}

Bytes makePes(std::uint8_t id, const Bytes& payload, std::int64_t pts = -1, std::int64_t dts = -1) {
  Bytes ts;
  if (pts >= 0) {
    appendTimestamp(ts, dts >= 0 ? 3 : 2, pts);
    if (dts >= 0) appendTimestamp(ts, 1, dts);
  }
  const unsigned flags = pts < 0 ? 0 : dts < 0 ? 2 : 3;
  const std::size_t length = 3 + ts.size() + payload.size();
  Bytes v = {0, 0, 1, id, static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length), 0x80,
             static_cast<std::uint8_t>(flags << 6), static_cast<std::uint8_t>(ts.size())};
  v.insert(v.end(), ts.begin(), ts.end());
  v.insert(v.end(), payload.begin(), payload.end());
  return v;
}

Bytes makeStreamPacket(std::uint8_t id, const Bytes& payload) {
  Bytes v = {0, 0, 1, id, static_cast<std::uint8_t>(payload.size() >> 8), static_cast<std::uint8_t>(payload.size())};
  v.insert(v.end(), payload.begin(), payload.end());
  return v;
}

// One 2048-byte pack: pack header, the packets, then a padding packet up to the pack end.
Bytes makePack(const std::vector<Bytes>& packets, bool pad = true, std::size_t stuffing = 0) {
  Bytes v = {0, 0, 1, 0xBA, 0x44, 0x00, 0x0F, 0x25, 0xC4, 0xC1, 0x07, 0x53, 0x03,
             static_cast<std::uint8_t>(0xF8 | stuffing)};
  v.insert(v.end(), stuffing, 0xFF);
  for (const Bytes& p : packets) v.insert(v.end(), p.begin(), p.end());
  if (pad && v.size() + 6 <= 2048) {
    const Bytes padding = makeStreamPacket(0xBE, Bytes(2048 - v.size() - 6, 0xFF));
    v.insert(v.end(), padding.begin(), padding.end());
  }
  v.resize(2048, 0);
  return v;
}

// `count` ATRAC3plus frames of 24 bytes (size code 1, 48 kHz, stereo); no sync pattern inside a payload.
Bytes makeAtracFrames(int count) {
  Bytes v;
  for (int i = 0; i < count; i++) {
    const Bytes header = {0x0F, 0xD0, 0x48, 0x01, 0, 0, 0, 0};
    v.insert(v.end(), header.begin(), header.end());
    v.insert(v.end(), 16, static_cast<std::uint8_t>(0x20 + i));
  }
  return v;
}

// Private stream 1 payload: sub-stream 0, a zero byte, then the continuation length.
Bytes makeAudioPayload(const Bytes& es, std::size_t from, std::size_t to, std::uint32_t continuation) {
  Bytes v = {0, 0, static_cast<std::uint8_t>(continuation >> 8), static_cast<std::uint8_t>(continuation)};
  v.insert(v.end(), es.begin() + static_cast<std::ptrdiff_t>(from), es.begin() + static_cast<std::ptrdiff_t>(to));
  return v;
}

struct DemuxResult {
  std::vector<std::vector<dal::PamfAccessUnit>> aus;  // per stream
  std::uint64_t errors = 0;
  std::size_t audio_before_end = 0;
};

void drainAll(dal::PamfDemuxer& demux, DemuxResult& r) {
  for (std::uint32_t i = 0; i < r.aus.size(); i++) {
    while (auto au = demux.next(i)) r.aus[i].push_back(std::move(*au));
  }
}

// Feeds `data` cut at the given offsets, or in pieces of `chunk` bytes when `cuts` is empty.
DemuxResult runDemux(const dal::PamfInfo& info, const Bytes& data, std::vector<std::size_t> cuts,
                     std::size_t chunk = 0, bool end_of_stream = true) {
  dal::PamfDemuxer demux(info);
  DemuxResult r;
  r.aus.resize(info.streams.size());
  cuts.push_back(data.size());
  std::size_t pos = 0, cut = 0;
  while (pos < data.size()) {
    const std::size_t stop = chunk != 0 ? std::min(data.size(), pos + chunk) : cuts[cut++];
    demux.feed(std::span<const std::uint8_t>(data).subspan(pos, stop - pos));
    pos = stop;
    drainAll(demux, r);
  }
  r.audio_before_end = r.aus.size() > 1 ? r.aus[1].size() : 0;
  if (end_of_stream) demux.endOfStream();
  drainAll(demux, r);
  r.errors = demux.errors();
  return r;
}

bool sameAus(const DemuxResult& a, const DemuxResult& b) {
  if (a.errors != b.errors || a.aus.size() != b.aus.size() || a.audio_before_end != b.audio_before_end) return false;
  for (std::size_t s = 0; s < a.aus.size(); s++) {
    if (a.aus[s].size() != b.aus[s].size()) return false;
    for (std::size_t i = 0; i < a.aus[s].size(); i++) {
      const dal::PamfAccessUnit &x = a.aus[s][i], &y = b.aus[s][i];
      if (x.stream_index != y.stream_index || x.pts != y.pts || x.dts != y.dts || x.bytes != y.bytes) return false;
    }
  }
  return true;
}

const Bytes kVideoPayload1 = {0, 0, 0, 1, 0x09, 0x10, 0xAA, 0xBB};
const Bytes kVideoPayload2 = {0xCC, 0xDD, 0xEE};
const Bytes kVideoPayload3 = {0, 0, 1, 0x09, 0x30};

// Video and ATRAC3plus packs as in the game files: a random access pack with system header and
// private stream 2, audio split across packets, and a final audio packet without a timestamp.
Bytes makeSyntheticStream(const Bytes& es) {
  const Bytes system_header = makeStreamPacket(0xBB, Bytes(6, 0x80));
  const Bytes private_stream_2 = makeStreamPacket(0xBF, {0x01, 0xE0, 0x00, 0x80, 0x01, 0x00});
  Bytes data;
  auto add = [&](const Bytes& pack) { data.insert(data.end(), pack.begin(), pack.end()); };
  add(makePack({system_header, private_stream_2, makePes(0xE0, kVideoPayload1, 90000, 86997)}));
  add(makePack({makePes(0xBD, makeAudioPayload(es, 0, 60, 0), 1000)}));
  add(makePack({makePes(0xE0, kVideoPayload2)}));
  // Frame 2 continues for 12 bytes here, frame 3 starts at byte 72, frame 4 at byte 96.
  add(makePack({makePes(0xBD, makeAudioPayload(es, 60, 100, 12), 20000)}));
  add(makePack({makePes(0xBD, makeAudioPayload(es, 100, es.size(), 20))}));
  add(makePack({makePes(0xE0, kVideoPayload3, 93003)}));
  return data;
}

void testPamfDemuxSynthetic() {
  const Bytes header = makePamfHeader(6);
  const auto info = dal::parsePamfHeader(header);
  CHECK(info.has_value());
  if (!info) return;
  const Bytes es = makeAtracFrames(6);
  const Bytes data = makeSyntheticStream(es);
  CHECK(data.size() == 6 * 2048);

  // Three feeds that cut through the middle of packs.
  const DemuxResult r = runDemux(*info, data, {1000, 5000});
  CHECK(r.errors == 0);
  CHECK(r.aus.size() == 2 && r.aus[0].size() == 3);
  if (r.aus.size() == 2 && r.aus[0].size() == 3) {
    CHECK(r.aus[0][0].pts == 90000 && r.aus[0][0].dts == 86997 && r.aus[0][0].bytes == kVideoPayload1);
    CHECK(r.aus[0][1].pts == -1 && r.aus[0][1].dts == -1 && r.aus[0][1].bytes == kVideoPayload2);
    CHECK(r.aus[0][2].pts == 93003 && r.aus[0][2].dts == -1 && r.aus[0][2].bytes == kVideoPayload3);
    CHECK(r.aus[0][0].stream_index == 0);
  }

  // Frame 5 ends exactly at the end of the data, so it is delivered only by endOfStream.
  CHECK(r.audio_before_end == 5);
  CHECK(r.aus[1].size() == 6);
  if (r.aus[1].size() == 6) {
    // Frames 0, 3: PES timestamps. Frames 1, 2, 4, 5: previous plus 3840 ticks.
    const std::int64_t expected[] = {1000, 4840, 8680, 20000, 23840, 27680};
    for (int i = 0; i < 6; i++) {
      CHECK(r.aus[1][i].pts == expected[i] && r.aus[1][i].dts == -1 && r.aus[1][i].stream_index == 1);
      CHECK(r.aus[1][i].bytes == Bytes(es.begin() + i * 24, es.begin() + (i + 1) * 24));
    }
  }

  // The result does not depend on how the input is chunked.
  for (std::size_t chunk : {1, 7, 100, 2047, 2048, 2049, 4096}) {
    CHECK(sameAus(runDemux(*info, data, {}, chunk), r));
  }
  CHECK(sameAus(runDemux(*info, data, {}, data.size()), r));
}

void testPamfDemuxDamage() {
  const auto info = dal::parsePamfHeader(makePamfHeader(8));
  CHECK(info.has_value());
  if (!info) return;
  const Bytes es = makeAtracFrames(6);
  const Bytes video_ok = makePes(0xE0, kVideoPayload1, 90000, 86997);

  // A PES length that runs past the pack end, a section without a pack start code and a pack with a
  // bad packet prefix are each counted once and skipped; the packs around them still demux.
  Bytes overrun = makePes(0xE0, kVideoPayload2);
  overrun[4] = 0x7F;
  Bytes bad_prefix = makePack({Bytes{0, 0, 2, 0xE0, 0, 3, 0x80, 0, 0}});
  Bytes garbage(2048, 0x5A);

  Bytes data;
  auto add = [&](const Bytes& pack) { data.insert(data.end(), pack.begin(), pack.end()); };
  add(makePack({video_ok}));
  add(makePack({overrun}, false));
  add(garbage);
  add(bad_prefix);
  add(makePack({makePes(0xBD, makeAudioPayload(es, 0, es.size(), 0), 500)}));
  add(makePack({makePes(0xE0, kVideoPayload3)}));

  for (std::size_t chunk : {0, 1, 1000, 2048}) {
    const DemuxResult r = runDemux(*info, data, {}, chunk != 0 ? chunk : data.size());
    CHECK(r.errors == 3);
    CHECK(r.aus[0].size() == 2 && r.aus[0][0].pts == 90000 && r.aus[0][1].bytes == kVideoPayload3);
    CHECK(r.aus[1].size() == 6 && r.aus[1][5].pts == 500 + 5 * 3840);
  }

  // A program end code delivers the pending last frame without endOfStream and is not an error.
  Bytes program_end = {0, 0, 1, 0xB9};
  program_end.resize(2048, 0);
  Bytes with_end;
  Bytes audio_pack = makePack({makePes(0xBD, makeAudioPayload(es, 0, es.size(), 0), 500)});
  with_end.insert(with_end.end(), audio_pack.begin(), audio_pack.end());
  with_end.insert(with_end.end(), program_end.begin(), program_end.end());
  const DemuxResult ended = runDemux(*info, with_end, {2048}, 0, false);
  CHECK(ended.errors == 0 && ended.aus[1].size() == 6);

  // A short last section is parsed as far as it goes and counts as one error, wherever it is cut off.
  Bytes truncated = makePack({video_ok});
  Bytes second = makePack({makePes(0xE0, kVideoPayload2)});
  truncated.insert(truncated.end(), second.begin(), second.begin() + 20);
  const DemuxResult cut = runDemux(*info, truncated, {2060});
  CHECK(cut.errors == 1 && cut.aus[0].size() == 1);
  Bytes header_only = makePack({video_ok});
  header_only.insert(header_only.end(), second.begin(), second.begin() + 14);  // ends between packets
  const DemuxResult cut_between = runDemux(*info, header_only, {});
  CHECK(cut_between.errors == 1 && cut_between.aus[0].size() == 1);

  // A damaged frame header costs one error, however long the resynchronisation takes. Frame 2 claims
  // 56 bytes instead of 24, so no sync word follows it; frames 0, 1, 3, 4 and 5 are still delivered.
  Bytes bad_es = es;
  bad_es[2 * 24 + 3] = 0x05;
  const Bytes bad_pack = makePack({makePes(0xBD, makeAudioPayload(bad_es, 0, bad_es.size(), 0), 500)});
  for (std::size_t chunk : {0, 1, 5, 100}) {
    const DemuxResult r = runDemux(*info, bad_pack, {}, chunk != 0 ? chunk : bad_pack.size());
    CHECK(r.errors == 1 && r.aus[1].size() == 5);
    if (r.aus[1].size() != 5) continue;
    const int expected_frames[] = {0, 1, 3, 4, 5};
    for (int i = 0; i < 5; i++) {
      const int f = expected_frames[i];
      CHECK(r.aus[1][i].bytes == Bytes(es.begin() + f * 24, es.begin() + (f + 1) * 24));
    }
  }

  // pack_stuffing_length is honoured, padding may come first, and a program end code may follow a PES
  // inside a pack: what came before it is delivered, the next section continues, and nothing is an error.
  for (std::size_t stuffing : {2, 7}) {
    const DemuxResult stuffed = runDemux(*info, makePack({video_ok}, true, stuffing), {});
    CHECK(stuffed.errors == 0 && stuffed.aus[0].size() == 1 && stuffed.aus[0][0].pts == 90000);
  }
  const DemuxResult padding_first = runDemux(*info, makePack({makeStreamPacket(0xBE, Bytes(10, 0xFF)), video_ok}), {});
  CHECK(padding_first.errors == 0 && padding_first.aus[0].size() == 1 && padding_first.aus[0][0].pts == 90000);

  Bytes in_pack = makePack({video_ok, makePes(0xBD, makeAudioPayload(es, 0, es.size(), 0), 500), Bytes{0, 0, 1, 0xB9}},
                           false);
  const Bytes after = makePack({makePes(0xE0, kVideoPayload3)});
  in_pack.insert(in_pack.end(), after.begin(), after.end());
  const DemuxResult program_end_in_pack = runDemux(*info, in_pack, {1500}, 0, false);
  CHECK(program_end_in_pack.errors == 0 && program_end_in_pack.aus[0].size() == 2 &&
        program_end_in_pack.aus[1].size() == 6);

  // A stream that is not in the table is skipped silently, a bad out-of-range index returns nothing.
  Bytes other = makePack({makePes(0xE3, kVideoPayload2), makePes(0xBD, Bytes{0x31, 0, 0, 0, 1, 2})});
  const DemuxResult skipped = runDemux(*info, other, {});
  CHECK(skipped.errors == 0 && skipped.aus[0].empty() && skipped.aus[1].empty());
  dal::PamfDemuxer demux(*info);
  CHECK(!demux.next(2).has_value() && demux.errors() == 0);

  // A PES with the MPEG-1 style header byte is damage.
  Bytes mpeg1 = makePes(0xE0, kVideoPayload2);
  mpeg1[6] = 0x0F;
  const DemuxResult bad_header = runDemux(*info, makePack({mpeg1}), {});
  CHECK(bad_header.errors == 1 && bad_header.aus[0].empty());
}

// Fake codecs for the player: one frame or chunk comes out per access unit, marked with its index, so
// the tests see exactly what the player did with it.
struct FakeShared {
  std::atomic<int> video_sent{0}, audio_sent{0};
  std::atomic<int> video_delay_ms{0};
  std::atomic<std::int64_t> format_start_pts{-12345};
  bool fail_video = false, fail_audio = false;
};

constexpr std::uint32_t kFakeChunkFrames = 2048;  // sample frames per fake ATRAC3plus chunk
constexpr std::uint32_t kFakeFrameBytes = 8;      // stereo float32

class FakeVideoDecoder : public dal::VideoDecoder {
 public:
  explicit FakeVideoDecoder(FakeShared* shared) : shared_(shared) {}
  bool send(std::span<const std::uint8_t>, std::int64_t pts, std::int64_t) override {
    if (const int delay = shared_->video_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    const int index = shared_->video_sent++;
    // Like the real decoder: a frame without a PTS continues after the previous one.
    last_pts_ = pts >= 0 ? pts : last_pts_ + 3003;
    dal::VideoFrame frame;
    frame.pts = last_pts_;
    frame.width = 4;
    frame.height = 2;
    frame.pitch = 16;
    frame.bytes.assign(32, static_cast<std::uint8_t>(index));
    pending_.push_back(std::move(frame));
    return true;
  }
  bool receive(dal::VideoFrame& out) override {
    if (pending_.empty()) return false;
    out = std::move(pending_.front());
    pending_.erase(pending_.begin());
    return true;
  }
  void flush() override {}

 private:
  FakeShared* shared_;
  std::int64_t last_pts_ = 0;
  std::vector<dal::VideoFrame> pending_;
};

class FakeAudioDecoder : public dal::AudioDecoder {
 public:
  explicit FakeAudioDecoder(FakeShared* shared) : shared_(shared) {}
  bool send(std::span<const std::uint8_t>, std::int64_t pts) override {
    const int index = shared_->audio_sent++;
    dal::AudioChunk chunk;
    chunk.pts = pts;
    chunk.frames = kFakeChunkFrames;
    chunk.bytes.assign(std::size_t{kFakeChunkFrames} * kFakeFrameBytes, static_cast<std::uint8_t>(index + 1));
    pending_.push_back(std::move(chunk));
    return true;
  }
  bool receive(dal::AudioChunk& out) override {
    if (pending_.empty()) return false;
    out = std::move(pending_.front());
    pending_.erase(pending_.begin());
    return true;
  }
  void flush() override {}

 private:
  FakeShared* shared_;
  std::vector<dal::AudioChunk> pending_;
};

class FakeFactory : public dal::MediaCodecFactory {
 public:
  explicit FakeFactory(FakeShared* shared) : shared_(shared) {}
  std::unique_ptr<dal::VideoDecoder> createVideo(const dal::PamfStream&, const dal::VideoOutputFormat& format,
                                                 std::string& error) override {
    shared_->format_start_pts = format.start_pts;
    if (shared_->fail_video) {
      error = "fake video failure";
      return nullptr;
    }
    return std::make_unique<FakeVideoDecoder>(shared_);
  }
  std::unique_ptr<dal::AudioDecoder> createAudio(const dal::PamfStream&, const dal::AudioOutputFormat&,
                                                 std::string& error) override {
    if (shared_->fail_audio) {
      error = "fake audio failure";
      return nullptr;
    }
    return std::make_unique<FakeAudioDecoder>(shared_);
  }

 private:
  FakeShared* shared_;
};

// Video PES i has PTS 89310 + 3003 i (the header's start PTS is 89310). The first `audio_packs`
// packs are each followed by an audio pack with two ATRAC3plus frames; the first frame starts at
// 85470, one frame before the start PTS, as in the game's movies.
Bytes makePlayerStream(int video_packs, int audio_packs) {
  Bytes data;
  auto add = [&](const Bytes& pack) { data.insert(data.end(), pack.begin(), pack.end()); };
  for (int i = 0; i < video_packs; i++) {
    add(makePack({makePes(0xE0, Bytes{0, 0, 0, 1, 0x09, 0x10, static_cast<std::uint8_t>(i)}, 89310 + 3003 * i)}));
    if (i < audio_packs) {
      const Bytes es = makeAtracFrames(2);
      add(makePack({makePes(0xBD, makeAudioPayload(es, 0, es.size(), 0), 85470 + 7680 * i)}));
    }
  }
  return data;
}

struct TempMovie {
  std::filesystem::path path;
  explicit TempMovie(const Bytes& data, const char* name = "dal_movie_player_test.pam") {
    std::error_code ec;
    path = std::filesystem::temp_directory_path(ec) / name;
    Bytes file = makePamfHeader(static_cast<std::uint32_t>(data.size() / 2048));
    file.insert(file.end(), data.begin(), data.end());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  }
  ~TempMovie() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

template <class Pred>
bool pollFor(Pred done, int timeout_ms = 5000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

constexpr std::int64_t kPlayerStartPts = 89310;
constexpr std::int64_t kPlayerDuration = 722910 - 89310;

struct PlayerFixture {
  FakeShared shared;
  std::atomic<std::int64_t> clock{1000};
  dal::MoviePlayerOptions options;
  PlayerFixture() {
    options.clock = [this] { return clock.load(); };
    dal::setMediaCodecFactory(std::make_unique<FakeFactory>(&shared));
  }
  ~PlayerFixture() { dal::setMediaCodecFactory(nullptr); }
};

bool decodeFinished(const dal::MoviePlayer& player) { return player.stats().decode_finished; }

void testMoviePlayerOrderAndTiming() {
  PlayerFixture fx;
  fx.options.video_queue_frames = 64;  // everything fits, so decoding ends without a consumer
  const TempMovie movie(makePlayerStream(10, 4));
  dal::MoviePlayer player(fx.options);

  CHECK(!player.start());
  CHECK(!player.open(std::filesystem::temp_directory_path() / "dal_movie_player_missing.pam", {}, {}).has_value());
  CHECK(!player.start());
  const auto info = player.open(movie.path, {}, {});
  CHECK(info.has_value());
  if (!info) return;
  CHECK(info->streams.size() == 2 && static_cast<std::int64_t>(info->start_pts) == kPlayerStartPts);
  CHECK(player.start());
  CHECK(!player.start());
  CHECK(player.waitPrerolled(std::chrono::seconds(5)));
  CHECK(pollFor([&] { return decodeFinished(player); }));
  // The decoder is told the file's start PTS even though the caller passed none.
  CHECK(fx.shared.format_start_pts == kPlayerStartPts);

  dal::MoviePlayerStats stats = player.stats();
  CHECK(stats.video_decoder && stats.audio_decoder && stats.codec_error.empty());
  CHECK(stats.video_decoded == 10 && stats.audio_decoded_frames == 8 * kFakeChunkFrames);
  CHECK(stats.demux_errors == 0 && stats.read_errors == 0);
  CHECK(stats.width == 1280 && stats.height == 720 && stats.duration_ticks == kPlayerDuration);
  CHECK(stats.frame_rate_num == 30000 && stats.frame_rate_den == 1001);  // the header states no rate

  const std::int64_t origin = 1000;
  std::int64_t last_pts = -1;
  int delivered = 0;
  std::uint32_t dropped = 0;
  auto expectFrame = [&](std::int64_t now, int index, std::uint32_t expected_dropped) {
    auto due = player.takeDueVideo(now);
    CHECK(due.has_value());
    if (!due) return;
    CHECK(due->frame.pts == kPlayerStartPts + 3003 * index);
    CHECK(!due->frame.bytes.empty() && due->frame.bytes[0] == index);
    CHECK(due->dropped == expected_dropped);
    CHECK(due->frame.pts > last_pts);
    last_pts = due->frame.pts;
    delivered++;
    dropped += due->dropped;
  };
  // Frame i is due 3003 i ticks after the origin; nothing before that.
  expectFrame(origin, 0, 0);
  CHECK(!player.takeDueVideo(origin + 3002).has_value());
  expectFrame(origin + 3003, 1, 0);
  CHECK(!player.takeDueVideo(origin + 3003 + 3002).has_value());
  // A jump over frames 2, 3 and 4 delivers the newest due frame and counts the skipped ones.
  expectFrame(origin + 5 * 3003 + 10, 5, 3);
  expectFrame(origin + 1000000, 9, 3);
  CHECK(!player.takeDueVideo(origin + 2000000).has_value());
  stats = player.stats();
  CHECK(stats.video_delivered == 4 && stats.video_dropped == 6 && delivered == 4 && dropped == 6);

  // The movie ends when the clock has covered the interval, whatever the queues hold.
  CHECK(!player.atEnd(origin + kPlayerDuration - 1));
  CHECK(player.atEnd(origin + kPlayerDuration));
  CHECK(player.stats().played_ticks == kPlayerDuration);

  // Audio keeps its own PTS on the video's origin, including the frame before the start PTS.
  const dal::MovieAudioBlock a = player.takeAudio(100);
  CHECK(a.pts == 85470 && a.pts < kPlayerStartPts && a.frames == 100 && a.real_frames == 100);
  CHECK(a.bytes.size() == 100 * kFakeFrameBytes && a.bytes[0] == 1);
  // A partial chunk advances the PTS by the samples taken (100 frames = 187.5 ticks, rounded down).
  const dal::MovieAudioBlock b = player.takeAudio(100);
  CHECK(b.pts == 85470 + 187 && b.real_frames == 100);
  // A block that spans three chunks: 1848 frames of chunk 0, all of chunk 1, 1104 frames of chunk 2.
  const dal::MovieAudioBlock c = player.takeAudio(5000);
  CHECK(c.pts == 85470 + 200 * 90000 / 48000 && c.frames == 5000 && c.real_frames == 5000);
  CHECK(c.bytes.size() == 5000 * kFakeFrameBytes);
  if (c.bytes.size() == 5000 * kFakeFrameBytes) {
    CHECK(c.bytes[0] == 1 && c.bytes[1847 * kFakeFrameBytes] == 1);
    CHECK(c.bytes[1848 * kFakeFrameBytes] == 2 && c.bytes[3895 * kFakeFrameBytes] == 2);
    CHECK(c.bytes[3896 * kFakeFrameBytes] == 3 && c.bytes[4999 * kFakeFrameBytes + 7] == 3);
  }
  // Asking for more than is left gives the rest and then zero-filled silence.
  const dal::MovieAudioBlock d = player.takeAudio(20000);
  const std::uint32_t left = 8 * kFakeChunkFrames - 5200;
  CHECK(d.frames == 20000 && d.real_frames == left);
  CHECK(d.bytes.size() == 20000 * kFakeFrameBytes && d.bytes[left * kFakeFrameBytes - 1] == 8);
  CHECK(d.bytes[left * kFakeFrameBytes] == 0 && d.bytes.back() == 0);
  // Silence continues where the previous block ended.
  const dal::MovieAudioBlock e = player.takeAudio(64);
  CHECK(e.real_frames == 0 && e.pts == d.pts + 20000 * 90000 / 48000);
  CHECK(e.bytes == Bytes(64 * kFakeFrameBytes, 0));
  stats = player.stats();
  CHECK(stats.audio_delivered_frames == 8 * kFakeChunkFrames);
  CHECK(stats.audio_silence_frames == 20000 - left + 64);

  // Re-opening resets the player.
  CHECK(player.open(movie.path, {}, {}).has_value());
  CHECK(player.stats().video_decoded == 0 && !player.stats().decode_finished);
  CHECK(!player.takeDueVideo(origin + 1000000).has_value());
}

void testMoviePlayerBackpressure() {
  PlayerFixture fx;
  const TempMovie movie(makePlayerStream(40, 0));
  dal::MoviePlayer player(fx.options);
  CHECK(player.open(movie.path, {}, {}).has_value());
  CHECK(player.start());

  // Nobody takes frames: the queue holds 4 and the decode thread holds one more, then it waits.
  CHECK(pollFor([&] { return fx.shared.video_sent >= 5; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(fx.shared.video_sent <= 6);
  CHECK(!decodeFinished(player));

  // The consumer is not held up by the blocked decode thread.
  const auto before = std::chrono::steady_clock::now();
  auto first = player.takeDueVideo(1000);
  const auto took = std::chrono::steady_clock::now() - before;
  CHECK(first.has_value() && first->frame.bytes[0] == 0);
  CHECK(took < std::chrono::milliseconds(500));
  CHECK(!decodeFinished(player));

  // Taking frames lets the thread go on until the end of the file.
  std::uint64_t seen = first ? 1 + first->dropped : 0;
  const std::int64_t far = 1000 + 100000000;
  CHECK(pollFor([&] {
    if (auto due = player.takeDueVideo(far)) seen += 1 + due->dropped;
    return decodeFinished(player);
  }));
  while (auto due = player.takeDueVideo(far)) seen += 1 + due->dropped;
  CHECK(seen == 40 && fx.shared.video_sent == 40);
  CHECK(player.stats().video_decoded == 40);
}

// Nobody pulls audio (the default cellAudio never does): the video decode must still reach the end.
void testMoviePlayerUnpulledAudio() {
  PlayerFixture fx;
  fx.options.read_chunk_bytes = 2048;  // many small reads, so a stalled read shows up
  fx.options.audio_queue_frames = 4096;
  const TempMovie movie(makePlayerStream(40, 20));
  dal::MoviePlayer player(fx.options);
  CHECK(player.open(movie.path, {}, {}).has_value());
  CHECK(player.start());

  // The clock runs past the whole movie, so every queued chunk is stale once the queue is over its soft
  // limit.
  const std::int64_t far = 1000 + 100000000;
  fx.clock = far;
  CHECK(pollFor([&] {
    player.takeDueVideo(far);
    return decodeFinished(player);
  }));
  const dal::MoviePlayerStats stats = player.stats();
  CHECK(stats.video_decoded == 40 && stats.audio_decoded_frames == 40 * kFakeChunkFrames);
  CHECK(stats.audio_stale_frames > 0 && stats.audio_overflow_frames == 0);

  // What is left is the newest PCM, in increasing PTS order.
  std::int64_t last_pts = -1;
  std::uint64_t real = 0;
  bool increasing = true;
  for (;;) {
    const dal::MovieAudioBlock block = player.takeAudio(1000);
    if (block.real_frames == 0) break;
    if (block.pts <= last_pts) increasing = false;
    last_pts = block.pts;
    real += block.real_frames;
  }
  CHECK(real > 0 && increasing);
  CHECK(real + stats.audio_stale_frames == stats.audio_decoded_frames);
}

// A stalled clock: nothing is stale, so only the hard cap bounds the queue.
void testMoviePlayerAudioHardCap() {
  PlayerFixture fx;
  fx.options.read_chunk_bytes = 2048;
  fx.options.audio_queue_frames = 4096;
  fx.options.audio_queue_cap_frames = 8192;
  const TempMovie movie(makePlayerStream(40, 20));
  dal::MoviePlayer player(fx.options);
  CHECK(player.open(movie.path, {}, {}).has_value());
  CHECK(player.start());
  CHECK(pollFor([&] {
    player.takeDueVideo(1000 + 100000000);  // frames only; the player's clock stays at the origin
    return decodeFinished(player);
  }));
  const dal::MoviePlayerStats stats = player.stats();
  CHECK(stats.audio_decoded_frames == 40 * kFakeChunkFrames);
  CHECK(stats.audio_stale_frames == 0 && stats.audio_overflow_frames > 0);
  // The lead is measured against the stalled clock: all of the audio ends beyond it.
  CHECK(stats.audio_max_lead_ticks > 0 && stats.audio_max_lead_at_ticks == 0);

  std::uint64_t real = 0;
  std::int64_t last_pts = -1;
  bool increasing = true;
  for (;;) {
    const dal::MovieAudioBlock block = player.takeAudio(1000);
    if (block.real_frames == 0) break;
    if (block.pts <= last_pts) increasing = false;
    last_pts = block.pts;
    real += block.real_frames;
  }
  CHECK(real > 0 && real <= 8192 && increasing);
  CHECK(real + stats.audio_overflow_frames == stats.audio_decoded_frames);
}

void testMoviePlayerStop() {
  const auto elapsedMs = [](std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
  };

  // Blocked on a full video queue.
  {
    PlayerFixture fx;
    const TempMovie movie(makePlayerStream(40, 0));
    dal::MoviePlayer player(fx.options);
    CHECK(player.open(movie.path, {}, {}).has_value());
    CHECK(player.start());
    CHECK(pollFor([&] { return fx.shared.video_sent >= 5; }));
    const auto before = std::chrono::steady_clock::now();
    player.stop();
    CHECK(elapsedMs(before) < 1000);
    CHECK(decodeFinished(player));
    CHECK(player.takeDueVideo(1000 + 100000000).has_value());  // what was queued stays readable
    player.stop();  // idempotent
  }

  // Busy in a slow decoder.
  {
    PlayerFixture fx;
    fx.options.video_queue_frames = 1000;
    fx.shared.video_delay_ms = 20;
    const TempMovie movie(makePlayerStream(200, 0));
    dal::MoviePlayer player(fx.options);
    CHECK(player.open(movie.path, {}, {}).has_value());
    CHECK(player.start());
    CHECK(pollFor([&] { return fx.shared.video_sent >= 2; }));
    const auto before = std::chrono::steady_clock::now();
    player.stop();
    CHECK(elapsedMs(before) < 500);
    CHECK(fx.shared.video_sent < 200);
  }

  // Destroyed without stop(), with the thread waiting on a full queue.
  {
    PlayerFixture fx;
    const TempMovie movie(makePlayerStream(40, 0));
    const auto before = std::chrono::steady_clock::now();
    {
      dal::MoviePlayer player(fx.options);
      CHECK(player.open(movie.path, {}, {}).has_value());
      CHECK(player.start());
      CHECK(pollFor([&] { return fx.shared.video_sent >= 5; }));
    }
    CHECK(elapsedMs(before) < 1500);
  }
}

void testMoviePlayerWithoutCodecs() {
  dal::setMediaCodecFactory(nullptr);
  const TempMovie movie(makePlayerStream(5, 2));
  std::atomic<std::int64_t> clock{100};
  dal::MoviePlayerOptions options;
  options.clock = [&] { return clock.load(); };
  dal::MoviePlayer player(options);

  dal::VideoOutputFormat video;
  video.alpha = 0x80;
  CHECK(player.open(movie.path, video, {}).has_value());
  CHECK(player.start());
  CHECK(player.waitPrerolled(std::chrono::seconds(5)));
  CHECK(pollFor([&] { return decodeFinished(player); }));
  dal::MoviePlayerStats stats = player.stats();
  CHECK(!stats.video_decoder && !stats.audio_decoder && stats.video_decoded == 0);
  CHECK(stats.codec_error.find("no codec factory") != std::string::npos);

  // One black frame at the start PTS, in the stream's size: ARGB with the format's alpha.
  auto black = player.takeDueVideo(100);
  CHECK(black.has_value());
  if (black) {
    const dal::VideoFrame& f = black->frame;
    CHECK(f.pts == kPlayerStartPts && f.width == 1280 && f.height == 720 && f.pitch == 5120);
    CHECK(f.bytes.size() == 5120u * 720u);
    if (f.bytes.size() == 5120u * 720u) {
      CHECK(f.bytes[0] == 0x80 && f.bytes[1] == 0 && f.bytes[2] == 0 && f.bytes[3] == 0);
      CHECK(f.bytes[4] == 0x80 && f.bytes[f.bytes.size() - 4] == 0x80 && f.bytes.back() == 0);
    }
  }
  CHECK(!player.takeDueVideo(100 + 10000000).has_value());

  // Silence for the whole movie: the PTS runs on from the start PTS.
  const dal::MovieAudioBlock a = player.takeAudio(480);
  CHECK(a.pts == kPlayerStartPts && a.frames == 480 && a.real_frames == 0);
  CHECK(a.bytes == Bytes(480 * kFakeFrameBytes, 0));
  CHECK(player.takeAudio(480).pts == kPlayerStartPts + 900);

  // The movie ends after its duration on the clock. Moving the origin later moves the end.
  CHECK(!player.atEnd(100 + kPlayerDuration - 1));
  CHECK(player.atEnd(100 + kPlayerDuration));
  clock = 200;
  player.restartClock();
  CHECK(!player.atEnd(100 + kPlayerDuration));
  CHECK(player.atEnd(200 + kPlayerDuration));

  // Other pixel formats; the same player opens another movie.
  video.coding = dal::VideoCoding::Rgba;
  CHECK(player.open(movie.path, video, {}).has_value());
  CHECK(player.start());
  CHECK(player.waitPrerolled(std::chrono::seconds(5)));
  black = player.takeDueVideo(clock);
  CHECK(black.has_value() && black->frame.bytes.size() == 5120u * 720u);
  if (black && black->frame.bytes.size() == 5120u * 720u) {
    CHECK(black->frame.bytes[0] == 0 && black->frame.bytes[3] == 0x80);
  }
  video.coding = dal::VideoCoding::Yuv420Planar;
  video.width = 16;
  video.height = 8;
  CHECK(player.open(movie.path, video, {}).has_value());
  CHECK(player.start());
  CHECK(player.waitPrerolled(std::chrono::seconds(5)));
  black = player.takeDueVideo(clock);
  CHECK(black.has_value() && black->frame.bytes.size() == 16u * 8u + 2u * 8u * 4u);
  if (black && black->frame.bytes.size() == 16u * 8u + 2u * 8u * 4u) {
    CHECK(black->frame.pitch == 16 && black->frame.bytes[0] == 16 && black->frame.bytes[127] == 16);
    CHECK(black->frame.bytes[128] == 128 && black->frame.bytes.back() == 128);
  }
}

void testMoviePlayerPartialCodecs() {
  // The video decoder cannot be created: black frame, but the audio still plays.
  {
    PlayerFixture fx;
    fx.shared.fail_video = true;
    const TempMovie movie(makePlayerStream(5, 2));
    dal::MoviePlayer player(fx.options);
    CHECK(player.open(movie.path, {}, {}).has_value());
    CHECK(player.start());
    CHECK(pollFor([&] { return decodeFinished(player); }));
    // Nothing takes audio, so decoding of the 4 chunks stays within the queue and finishes.
    const dal::MoviePlayerStats stats = player.stats();
    CHECK(!stats.video_decoder && stats.audio_decoder);
    CHECK(stats.codec_error.find("fake video failure") != std::string::npos);
    auto black = player.takeDueVideo(1000);
    CHECK(black.has_value() && black->frame.pts == kPlayerStartPts);
    const dal::MovieAudioBlock a = player.takeAudio(300);
    CHECK(a.real_frames == 300 && a.pts == 85470);
  }
  // The audio decoder cannot be created: video plays, audio is silence.
  {
    PlayerFixture fx;
    fx.shared.fail_audio = true;
    fx.options.video_queue_frames = 64;  // no consumer here
    const TempMovie movie(makePlayerStream(5, 2));
    dal::MoviePlayer player(fx.options);
    CHECK(player.open(movie.path, {}, {}).has_value());
    CHECK(player.start());
    CHECK(pollFor([&] { return decodeFinished(player); }));
    const dal::MoviePlayerStats stats = player.stats();
    CHECK(stats.video_decoder && !stats.audio_decoder && stats.video_decoded == 5);
    const dal::MovieAudioBlock a = player.takeAudio(300);
    CHECK(a.real_frames == 0 && a.frames == 300);
  }
  // A file that is not PAMF is refused by open().
  {
    const TempMovie garbage(Bytes(4096, 0x5A), "dal_movie_player_garbage.pam");
    dal::MoviePlayer player;
    std::ofstream(garbage.path, std::ios::binary | std::ios::trunc) << "not a movie";
    const auto info = player.open(garbage.path, {}, {});
    CHECK(!info.has_value() && !info.error().empty());
  }
}

struct GameFile {
  const char* name;
  // The first three counts are from the whole-file statistics of the format description;
  // `video_pts_only` (PTS without DTS) was found by running the demuxer.
  std::uint32_t packs, video_pes, video_pts, video_pts_only;
};

const GameFile kGameFiles[] = {
    {"ch_logo.pam", 4037, 3979, 17, 0},   {"if_logo.pam", 4169, 4110, 17, 0},  {"op.pam", 113633, 112864, 241, 2},
    {"world.pam", 51111, 50434, 195, 0},  {"end01.pam", 78080, 77059, 293, 0}, {"end02.pam", 77956, 76935, 293, 0},
    {"end03.pam", 78023, 77002, 293, 0},  {"end04.pam", 77670, 76649, 292, 0}, {"end05.pam", 78067, 77046, 293, 0},
    {"end06.pam", 79860, 78813, 299, 2},
};

void testPamfGameFiles() {
  // _dupenv_s: the CRT marks getenv as deprecated.
  char* dir = nullptr;
  std::size_t dir_len = 0;
  if (_dupenv_s(&dir, &dir_len, "DAL_GAME_DIR") != 0 || dir == nullptr || *dir == '\0') {
    std::free(dir);
    std::printf("pamf game files: skipped (DAL_GAME_DIR is not set)\n");
    return;
  }
  const std::filesystem::path movie_dir = std::filesystem::path(dir) / "PS3_GAME" / "USRDIR" / "Movie";
  std::free(dir);

  for (const GameFile& file : kGameFiles) {
    const std::filesystem::path path = movie_dir / file.name;
    std::ifstream in(path, std::ios::binary);
    CHECK(in.is_open());
    if (!in.is_open()) {
      std::fprintf(stderr, "cannot open %s\n", file.name);
      continue;
    }
    Bytes header(2048);
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const auto info = dal::parsePamfHeader(header);
    CHECK(info.has_value());
    if (!info) {
      std::fprintf(stderr, "%s: %s\n", file.name, info.error().c_str());
      continue;
    }
    const bool is_ch_logo = std::string_view(file.name) == "ch_logo.pam";
    CHECK(info->streams.size() == 2 && info->streams[0].codec == dal::PamfCodec::Avc &&
          info->streams[1].codec == dal::PamfCodec::Atrac3plus);
    CHECK(info->data_offset == 2048 && info->data_size / 2048 == file.packs);
    CHECK(std::filesystem::file_size(path) == std::uint64_t{info->data_offset} + info->data_size);
    if (info->streams.size() != 2) continue;
    CHECK(info->streams[0].width == 1280 && info->streams[0].height == 720);
    CHECK(info->streams[1].channels == 2 && info->streams[1].sample_rate == 48000);
    if (is_ch_logo) {
      CHECK(info->start_pts == 89310 && info->end_pts == 722910 && info->mux_rate_bound == 120000);
      CHECK(info->streams[0].stream_id == 0xE0 && info->streams[1].stream_id == 0xBD &&
            info->streams[1].private_stream_id == 0);
    }

    // The whole data area, fed in odd-sized pieces so that packs and packets straddle feeds.
    dal::PamfDemuxer demux(*info);
    in.seekg(info->data_offset);
    Bytes chunk((1u << 20) + 13);
    std::uint64_t video_aus = 0, video_pts = 0, video_dts = 0, audio_frames = 0, audio_bytes = 0;
    std::int64_t last_video_pts = -1, last_video_dts = -1, last_audio_pts = -1, first_audio_pts = -1;
    std::int64_t first_video_pts = -1, first_video_dts = -1;
    std::uint64_t video_pts_only = 0;
    bool video_order_ok = true, audio_order_ok = true, audio_all_stamped = true;
    bool frames_ok = true;
    std::size_t first_frame_size = 0;
    auto pull = [&] {
      while (auto au = demux.next(0)) {
        video_aus++;
        if (au->pts >= 0) {
          video_pts++;
          if (au->pts <= last_video_pts) video_order_ok = false;
          last_video_pts = au->pts;
          if (first_video_pts < 0) {
            first_video_pts = au->pts;
            first_video_dts = au->dts;
          }
          if (au->dts < 0) {
            // The description lists a DTS for every PTS, but some op and end06 packets carry only a PTS.
            if (video_pts_only++ < 3) {
              std::printf("pamf %s: video AU %llu has PTS %lld and no DTS (last DTS %lld)\n", file.name,
                          static_cast<unsigned long long>(video_aus), static_cast<long long>(au->pts),
                          static_cast<long long>(last_video_dts));
            }
          }
        }
        if (au->dts >= 0) {
          video_dts++;
          if (au->dts <= last_video_dts) video_order_ok = false;
          last_video_dts = au->dts;
        }
      }
      while (auto au = demux.next(1)) {
        audio_frames++;
        audio_bytes += au->bytes.size();
        if (au->pts < 0) audio_all_stamped = false;
        else if (au->pts <= last_audio_pts) audio_order_ok = false;
        if (au->pts >= 0) last_audio_pts = au->pts;
        if (first_audio_pts < 0) {
          first_audio_pts = au->pts;
          first_frame_size = au->bytes.size();
        }
        if (au->bytes.size() < 8 || au->bytes[0] != 0x0F || au->bytes[1] != 0xD0) frames_ok = false;
      }
    };
    while (in) {
      in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
      const std::streamsize got = in.gcount();
      if (got <= 0) break;
      demux.feed(std::span<const std::uint8_t>(chunk).first(static_cast<std::size_t>(got)));
      pull();
    }
    demux.endOfStream();
    pull();

    std::printf(
        "pamf %s: packs %u, video PES/AUs %llu (PTS %llu, DTS %llu), audio frames %llu (%llu bytes), errors %llu, "
        "first video PTS/DTS %lld/%lld, audio PTS %lld..%lld\n",
        file.name, file.packs, static_cast<unsigned long long>(video_aus), static_cast<unsigned long long>(video_pts),
        static_cast<unsigned long long>(video_dts), static_cast<unsigned long long>(audio_frames),
        static_cast<unsigned long long>(audio_bytes), static_cast<unsigned long long>(demux.errors()),
        static_cast<long long>(first_video_pts), static_cast<long long>(first_video_dts),
        static_cast<long long>(first_audio_pts), static_cast<long long>(last_audio_pts));

    CHECK(demux.errors() == 0);
    CHECK(video_aus == file.video_pes && video_pts == file.video_pts);
    CHECK(video_pts_only == file.video_pts_only && video_dts == file.video_pts - file.video_pts_only);
    CHECK(video_order_ok && audio_order_ok && audio_all_stamped && frames_ok);
    CHECK(audio_frames > 0 && audio_bytes % first_frame_size == 0);
    if (is_ch_logo || std::string_view(file.name) == "if_logo.pam" || std::string_view(file.name) == "op.pam") {
      CHECK(first_video_pts == 90000 && first_video_dts == 86997);
    }
    if (is_ch_logo) {
      // One frame before the header start PTS, 165 inside the interval, and one starting at its end.
      CHECK(audio_frames == 167 && first_audio_pts == 85470 && last_audio_pts == 722910 && first_frame_size == 696);
    }
    if (std::string_view(file.name) == "op.pam") CHECK(last_audio_pts == 8629470);
  }
}

}  // namespace

int main() {
  dal::installProcessSettings();
  testGuestLayouts();
  testCodecFactory();
  testSailService();
  testPamfHeader();
  testPamfDemuxSynthetic();
  testPamfDemuxDamage();
  testMoviePlayerOrderAndTiming();
  testMoviePlayerBackpressure();
  testMoviePlayerUnpulledAudio();
  testMoviePlayerAudioHardCap();
  testMoviePlayerStop();
  testMoviePlayerWithoutCodecs();
  testMoviePlayerPartialCodecs();
  testPamfGameFiles();
  std::printf("media core tests: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed != 0 ? 1 : 0;
}
