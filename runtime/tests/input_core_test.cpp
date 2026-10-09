// Self-checking unit tests for the gamepad code in the runtime core. A fake backend stands in for
// GameInput, so nothing here needs a pad, a Windows input runtime or generated code.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/log.h"
#include "input/gamepad_backend.h"
#include "input/gamepad_probe.h"

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

using dal::GamepadButton;
using dal::gamepadButtonBit;

// A scriptable GamepadBackend: tests set the device list and the readings, and read the counters.
class FakeBackend final : public dal::GamepadBackend {
 public:
  std::string description() const override { return "fake"; }

  std::uint64_t deviceGeneration() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
  }

  std::vector<dal::GamepadDeviceInfo> devices() override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<dal::GamepadDeviceInfo> list;
    for (const Entry& entry : entries_) list.push_back(entry.info);
    return list;
  }

  std::optional<dal::GamepadReading> read(std::uint64_t id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& entry : entries_) {
      if (entry.info.id != id) continue;
      ++entry.reads;
      return entry.reading;
    }
    return std::nullopt;
  }

  void setRumble(std::uint64_t id, const dal::GamepadRumble& rumble) override {
    std::lock_guard<std::mutex> lock(mutex_);
    rumble_calls_.emplace_back(id, rumble);
  }

  // Plugs in a device and returns its id.
  std::uint64_t add(std::uint16_t vendor_id, std::uint16_t product_id, std::uint32_t kinds = dal::kGamepadKindGamepad) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry entry;
    entry.info.id = next_id_++;
    entry.info.vendor_id = vendor_id;
    entry.info.product_id = product_id;
    entry.info.kinds = kinds;
    entries_.push_back(entry);
    ++generation_;
    return entry.info.id;
  }

  void remove(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(entries_, [&](const Entry& e) { return e.info.id == id; });
    ++generation_;
  }

  // nullopt: the device has no reading.
  void setReading(std::uint64_t id, std::optional<dal::GamepadReading> reading) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& entry : entries_) {
      if (entry.info.id == id) entry.reading = reading;
    }
  }

 private:
  struct Entry {
    dal::GamepadDeviceInfo info;
    std::optional<dal::GamepadReading> reading = dal::GamepadReading{};
    int reads = 0;
  };

  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
  std::uint64_t next_id_ = 1;
  std::uint64_t generation_ = 0;
  std::vector<std::pair<std::uint64_t, dal::GamepadRumble>> rumble_calls_;
};

// Collects log lines on any thread while it is alive.
class LogCapture {
 public:
  LogCapture() {
    dal::setLogSink([this](std::string_view line) {
      std::lock_guard<std::mutex> lock(mutex_);
      lines_.emplace_back(line);
    });
  }
  ~LogCapture() { dal::setLogSink({}); }
  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;

  std::vector<std::string> lines() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
  }
  // Number of lines that contain `text`.
  int count(std::string_view text) const {
    int n = 0;
    for (const std::string& line : lines()) n += line.find(text) != std::string::npos ? 1 : 0;
    return n;
  }
  // Polls until a line containing `text` exists; the sink is global, so waits are bounded.
  bool waitFor(std::string_view text, std::chrono::milliseconds limit = std::chrono::seconds(5)) const {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
      if (count(text) > 0) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> lines_;
};

void testFormatValue() {
  CHECK(dal::formatGamepadValue(0.0f) == "0.00");
  CHECK(dal::formatGamepadValue(-0.0f) == "0.00");
  // A tiny negative value rounds to zero without keeping its sign.
  CHECK(dal::formatGamepadValue(-0.01f) == "0.00");
  CHECK(dal::formatGamepadValue(0.01f) == "0.00");
  CHECK(dal::formatGamepadValue(0.024f) == "0.00");
  // 0.049 is inside the step that rounds to 0.05; 0.026 is just over the boundary.
  CHECK(dal::formatGamepadValue(0.049f) == "0.05");
  CHECK(dal::formatGamepadValue(0.026f) == "0.05");
  CHECK(dal::formatGamepadValue(0.074f) == "0.05");
  CHECK(dal::formatGamepadValue(0.076f) == "0.10");
  CHECK(dal::formatGamepadValue(0.5f) == "0.50");
  CHECK(dal::formatGamepadValue(1.0f) == "1.00");
  CHECK(dal::formatGamepadValue(-1.0f) == "-1.00");
  CHECK(dal::formatGamepadValue(-0.049f) == "-0.05");
}

void testFormatReading() {
  CHECK(dal::formatGamepadReading({}) == "buttons none lt 0.00 rt 0.00 ls (0.00,0.00) rs (0.00,0.00)");

  dal::GamepadReading reading;
  reading.buttons = gamepadButtonBit(GamepadButton::A) | gamepadButtonBit(GamepadButton::RightShoulder);
  reading.left_trigger = 0.5f;
  reading.right_trigger = 1.0f;
  reading.left_x = -0.25f;
  reading.left_y = 0.999f;
  reading.right_x = -0.0f;
  reading.right_y = -1.0f;
  CHECK(dal::formatGamepadReading(reading) ==
        "buttons A+RightShoulder lt 0.50 rt 1.00 ls (-0.25,1.00) rs (0.00,-1.00)");

  // Every button has its own name, in the order of the enum.
  const char* const names[] = {"Menu",         "View",      "A",         "B",         "X",
                               "Y",            "DPadUp",    "DPadDown",  "DPadLeft",  "DPadRight",
                               "LeftShoulder", "RightShoulder", "LeftThumbstick", "RightThumbstick"};
  bool names_match = true;
  for (unsigned i = 0; i < static_cast<unsigned>(GamepadButton::Count); ++i) {
    dal::GamepadReading single;
    single.buttons = gamepadButtonBit(static_cast<GamepadButton>(i));
    const std::string expected = std::string("buttons ") + names[i] + " lt 0.00 rt 0.00 ls (0.00,0.00) rs (0.00,0.00)";
    names_match = names_match && dal::formatGamepadReading(single) == expected;
  }
  CHECK(names_match);
}

void testFormatDeviceParts() {
  CHECK(dal::formatGamepadKinds(dal::kGamepadKindGamepad) == "gamepad");
  CHECK(dal::formatGamepadKinds(dal::kGamepadKindController) == "controller");
  CHECK(dal::formatGamepadKinds(dal::kGamepadKindGamepad | dal::kGamepadKindController) == "gamepad+controller");
  CHECK(dal::formatGamepadKinds(0) == "none");

  dal::GamepadDeviceInfo info;
  info.vendor_id = 0x045e;
  info.product_id = 0x028e;
  CHECK(dal::formatGamepadVidPid(info) == "045e:028e");
  info.vendor_id = 0x54c;
  info.product_id = 0x9;
  CHECK(dal::formatGamepadVidPid(info) == "054c:0009");

  dal::SteamContext steam;
  CHECK(dal::formatSteamContext(steam) == "overlay not loaded");
  steam.overlay_loaded = true;
  CHECK(dal::formatSteamContext(steam) == "overlay loaded");
  steam.env["SteamGameId"] = "123";
  steam.env["SteamVirtualGamepadInfo"] = "a-file-path";
  steam.env["EnableConfiguratorSupport"] = "15";
  // std::map order: names sort alphabetically; the file path never appears.
  CHECK(dal::formatSteamContext(steam) ==
        "overlay loaded; EnableConfiguratorSupport=15 SteamGameId=123 SteamVirtualGamepadInfo=set");
}

void testProbeLogsChangesOnly() {
  LogCapture log;
  auto backend = std::make_shared<FakeBackend>();
  const std::uint64_t id = backend->add(0x045e, 0x028e);
  dal::GamepadProbe probe(backend);

  probe.step();
  CHECK(log.lines().size() == 1);
  CHECK(log.count("dal: pad: probe 1 045e:028e buttons none lt 0.00 rt 0.00 ls (0.00,0.00) rs (0.00,0.00)") == 1);

  // The same reading, and a change below the 0.05 rounding step, print nothing.
  probe.step();
  dal::GamepadReading reading;
  reading.left_x = 0.01f;
  backend->setReading(id, reading);
  probe.step();
  CHECK(log.lines().size() == 1);

  reading.buttons = gamepadButtonBit(GamepadButton::A);
  reading.left_x = 0.5f;
  backend->setReading(id, reading);
  probe.step();
  CHECK(log.lines().size() == 2);
  CHECK(log.count("dal: pad: probe 1 045e:028e buttons A lt 0.00 rt 0.00 ls (0.50,0.00) rs (0.00,0.00)") == 1);
  probe.step();
  CHECK(log.lines().size() == 2);

  // Going back to the first text prints again: the comparison is with the last printed one.
  backend->setReading(id, dal::GamepadReading{});
  probe.step();
  CHECK(log.lines().size() == 3);
}

void testProbeReportsMissingReadingOnce() {
  LogCapture log;
  auto backend = std::make_shared<FakeBackend>();
  const std::uint64_t pad = backend->add(0x045e, 0x028e);
  const std::uint64_t other = backend->add(0x054c, 0x05c4, dal::kGamepadKindController);
  backend->setReading(pad, std::nullopt);
  backend->setReading(other, std::nullopt);
  dal::GamepadProbe probe(backend);

  probe.step();
  probe.step();
  probe.step();
  CHECK(log.lines().size() == 2);
  CHECK(log.count("dal: pad: probe 1 045e:028e no gamepad reading") == 1);
  CHECK(log.count("dal: pad: probe 2 054c:05c4 no gamepad reading") == 1);

  // A reading that arrives later is an ordinary change.
  backend->setReading(pad, dal::GamepadReading{});
  probe.step();
  CHECK(log.lines().size() == 3);
  CHECK(log.count("dal: pad: probe 1 045e:028e buttons none") == 1);
}

void testProbeFollowsDevices() {
  LogCapture log;
  auto backend = std::make_shared<FakeBackend>();
  dal::GamepadProbe probe(backend);

  probe.step();
  CHECK(log.lines().empty());  // no device, no line

  const std::uint64_t first = backend->add(0x045e, 0x028e);
  const std::uint64_t second = backend->add(0x054c, 0x09cc);
  probe.step();
  CHECK(log.lines().size() == 2);

  // Each device keeps its own last text.
  dal::GamepadReading reading;
  reading.buttons = gamepadButtonBit(GamepadButton::B);
  backend->setReading(second, reading);
  probe.step();
  CHECK(log.lines().size() == 3);
  CHECK(log.count("dal: pad: probe 2 054c:09cc buttons B") == 1);

  // A removed device is forgotten; a new one (new id) prints its first reading again.
  backend->remove(first);
  const std::uint64_t third = backend->add(0x045e, 0x028e);
  CHECK(third != first);
  probe.step();
  CHECK(log.lines().size() == 4);
  CHECK(log.count("dal: pad: probe 3 045e:028e buttons none") == 1);
}

void testSteamVirtualGamepadInfoLog() {
  {
    LogCapture log;
    dal::logSteamVirtualGamepadInfo({});
    CHECK(log.lines().size() == 1);
    CHECK(log.count("dal: pad: probe SteamVirtualGamepadInfo unset") == 1);
  }

  const std::filesystem::path file = std::filesystem::temp_directory_path() /
      std::format("dal_input_core_test_vgp_info_{}.txt",
                  std::chrono::steady_clock::now().time_since_epoch().count());  // unique under ctest -j
  {
    std::ofstream out(file, std::ios::binary);
    out << "[slot 0]\r\nvid=0x045e\n\npid=0x028e";
  }
  {
    LogCapture log;
    dal::SteamContext steam;
    steam.env["SteamVirtualGamepadInfo"] = file.string();
    dal::logSteamVirtualGamepadInfo(steam);
    // Lines are split at \n, a trailing \r is dropped, and the last line needs no newline.
    CHECK(log.lines().size() == 4);
    CHECK(log.count("dal: pad: probe SteamVirtualGamepadInfo: [slot 0]") == 1);
    CHECK(log.count("dal: pad: probe SteamVirtualGamepadInfo: vid=0x045e") == 1);
    CHECK(log.count("dal: pad: probe SteamVirtualGamepadInfo: pid=0x028e") == 1);
  }
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << std::string(10000, 'x');
  }
  {
    LogCapture log;
    dal::SteamContext steam;
    steam.env["SteamVirtualGamepadInfo"] = file.string();
    dal::logSteamVirtualGamepadInfo(steam);
    // Only the first 4 KiB are read.
    CHECK(log.lines().size() == 1);
    CHECK(log.lines().front().size() < 4096 + 100);
  }
  {
    LogCapture log;
    dal::SteamContext steam;
    steam.env["SteamVirtualGamepadInfo"] = (file.parent_path() / "dal_input_core_test_missing.txt").string();
    dal::logSteamVirtualGamepadInfo(steam);
    CHECK(log.count("dal: pad: probe SteamVirtualGamepadInfo unreadable") == 1);
  }
  std::filesystem::remove(file);
}

void testProbeUnavailableBackend() {
  LogCapture log;
  dal::startGamepadProbe(
      []() -> std::expected<std::shared_ptr<dal::GamepadBackend>, std::string> {
        return std::unexpected(std::string("no runtime"));
      },
      [] { return dal::SteamContext{}; });
  CHECK(log.waitFor("dal: pad: gameinput unavailable: no runtime; gamepads disabled"));
}

// The probe thread never ends, so this runs last and the program leaves with _Exit (like the game).
// The fake's reading never changes, so the thread is silent after its start lines; that is what makes
// ~LogCapture's unsynchronised sink swap safe.
void testProbeThreadStartLines() {
  LogCapture log;
  auto backend = std::make_shared<FakeBackend>();
  backend->add(0x045e, 0x028e);
  dal::startGamepadProbe(
      [backend]() -> std::expected<std::shared_ptr<dal::GamepadBackend>, std::string> { return backend; },
      [] {
        dal::SteamContext steam;
        steam.overlay_loaded = true;
        steam.env["SteamGameId"] = "42";
        return steam;
      });
  CHECK(log.waitFor("dal: pad: probe 1 045e:028e buttons none"));
  const std::vector<std::string> lines = log.lines();
  CHECK(lines.size() == 4);
  if (lines.size() == 4) {
    CHECK(lines[0].ends_with("dal: pad: gameinput fake"));
    CHECK(lines[1].ends_with("dal: pad: steam overlay loaded; SteamGameId=42"));
    CHECK(lines[2].ends_with("dal: pad: probe SteamVirtualGamepadInfo unset"));
    CHECK(lines[3].ends_with("dal: pad: probe 1 045e:028e buttons none lt 0.00 rt 0.00 ls (0.00,0.00) rs (0.00,0.00)"));
  }
}

}  // namespace

int main() {
  testFormatValue();
  testFormatReading();
  testFormatDeviceParts();
  testProbeLogsChangesOnly();
  testProbeReportsMissingReadingOnce();
  testProbeFollowsDevices();
  testSteamVirtualGamepadInfoLog();
  testProbeUnavailableBackend();
  testProbeThreadStartLines();

  std::printf("input core tests: %d passed, %d failed\n", g_passed, g_failed);
  std::fflush(stdout);
  // Detached probe threads still run; leave without static destruction, as the game does.
  std::_Exit(g_failed == 0 ? 0 : 1);
}
