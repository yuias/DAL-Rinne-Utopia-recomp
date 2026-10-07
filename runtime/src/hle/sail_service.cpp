#include "hle/sail_service.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <optional>

#include "core/boot.h"
#include "core/host_functions.h"
#include "core/log.h"
#include "core/threads.h"
#include "media/movie_decoder.h"

namespace dal {
namespace {

// Priorities are only stored (see GuestLock). The stack is larger than the GCM interrupt thread's
// because the game's allocation callbacks run here and may call into its heap.
constexpr std::int32_t kThreadPriority = 1001;
constexpr std::uint32_t kThreadStackSize = 0x10000;
constexpr char kThreadName[] = "_libsail-control";

// Everything is guarded by the guest lock.
struct ServiceState {
  std::condition_variable_any wake;
  std::deque<SailCall> queue;
  bool stop = false;
  std::uint32_t thread_id = 0;
};

ServiceState& serviceState() {
  // Leaked: the thread may still be waiting on it while the process exits.
  static ServiceState* state = new ServiceState;
  return *state;
}

// Entry function of the thread. Returning ends the thread (only stopSailService asks for it).
void serviceThreadBody(DalPpuContext* ctx, std::uint8_t* base) {
  ServiceState& s = serviceState();
  for (;;) {
    blockingWait(
        s.wake, [&] { return s.stop || !s.queue.empty(); }, "sail control wait", std::nullopt);
    if (s.stop) {
      ctx->gpr[3] = 0;
      return;
    }
    SailCall call = std::move(s.queue.front());
    s.queue.pop_front();
    if (call) call(ctx, base);
    // Give the guest threads that queued meanwhile a turn before the next call.
    yieldGuestLock();
  }
}

// Written once at start-up, before any movie opens. The pointer is published with release so the
// decode threads, which never take the guest lock, can load it.
struct FactoryHolder {
  std::atomic<MediaCodecFactory*> current{nullptr};
  std::unique_ptr<MediaCodecFactory> owned;
};

FactoryHolder& factoryHolder() {
  // Leaked: a decode thread may still use the factory while static destructors run.
  static FactoryHolder* holder = new FactoryHolder;
  return *holder;
}

bool startServiceThread(ServiceState& s) {
  const std::uint32_t r13 = allocateTls();
  if (r13 == 0) return false;

  GuestThreadParams params;
  params.entry_opd = registerHostFunction("sailServiceThread", serviceThreadBody);
  params.priority = kThreadPriority;
  params.stack_size = kThreadStackSize;
  params.tls_r13 = r13;
  params.service = true;
  params.name = kThreadName;
  const std::uint32_t id = createGuestThread(params);
  if (id == 0) return false;

  s.stop = false;
  s.thread_id = id;
  return true;
}

}  // namespace

void queueSailCall(SailCall call) {
  ServiceState& s = serviceState();
  if (s.thread_id == 0 && !startServiceThread(s)) {
    // The game would wait for events that never come; say so instead of failing silently.
    logOnce(LogLevel::Error, "sail-service-thread",
            "dal: sail: cannot create the {} thread; a queued call is dropped", kThreadName);
    return;
  }
  s.queue.push_back(std::move(call));
  s.wake.notify_one();
}

void stopSailService() {
  ServiceState& s = serviceState();
  if (s.thread_id != 0) {
    GuestThread* thread = findThread(s.thread_id);
    s.stop = true;
    s.wake.notify_all();
    if (thread != nullptr) {
      blockingWait(
          thread->joiners(), [&] { return thread->state() == ThreadState::Exited; }, "sail service stop",
          std::chrono::seconds(5));
    }
  }
  s.queue.clear();
  s.stop = false;
  s.thread_id = 0;
}

void setMediaCodecFactory(std::unique_ptr<MediaCodecFactory> factory) {
  FactoryHolder& holder = factoryHolder();
  holder.current.store(factory.get(), std::memory_order_release);
  holder.owned = std::move(factory);
}

MediaCodecFactory* mediaCodecFactory() {
  return factoryHolder().current.load(std::memory_order_acquire);
}

}  // namespace dal
