// Self-checking tests for the movie pipeline's core: cellSail guest layouts, codec interfaces and the
// service thread. No FFmpeg and no game data are involved.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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

}  // namespace

int main() {
  dal::installProcessSettings();
  testGuestLayouts();
  testCodecFactory();
  testSailService();
  std::printf("media core tests: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed != 0 ? 1 : 0;
}
