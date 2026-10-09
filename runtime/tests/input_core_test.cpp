// Self-checking unit tests for the gamepad code in the runtime core. A fake backend stands in for
// GameInput, so nothing here needs a pad, a Windows input runtime or generated code.

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/log.h"
#include "input/gamepad_backend.h"
#include "input/gamepad_map.h"
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

dal::GamepadReading readingWithButton(GamepadButton button) {
  dal::GamepadReading reading;
  reading.buttons = gamepadButtonBit(button);
  return reading;
}

void testMapButtonTable() {
  struct Row {
    GamepadButton from;
    dal::PadButton to;
  };
  const Row rows[] = {
      {GamepadButton::A, dal::PadButton::Cross},
      {GamepadButton::B, dal::PadButton::Circle},
      {GamepadButton::X, dal::PadButton::Square},
      {GamepadButton::Y, dal::PadButton::Triangle},
      {GamepadButton::DPadUp, dal::PadButton::Up},
      {GamepadButton::DPadDown, dal::PadButton::Down},
      {GamepadButton::DPadLeft, dal::PadButton::Left},
      {GamepadButton::DPadRight, dal::PadButton::Right},
      {GamepadButton::LeftShoulder, dal::PadButton::L1},
      {GamepadButton::RightShoulder, dal::PadButton::R1},
      {GamepadButton::View, dal::PadButton::Select},
      {GamepadButton::Menu, dal::PadButton::Start},
      {GamepadButton::LeftThumbstick, dal::PadButton::L3},
      {GamepadButton::RightThumbstick, dal::PadButton::R3},
  };
  static_assert(std::size(rows) == static_cast<std::size_t>(GamepadButton::Count));
  for (const Row& row : rows) {
    const dal::MappedPad pad = dal::mapGamepadReading(readingWithButton(row.from));
    CHECK(pad.buttons == dal::padButtonBit(row.to));
    CHECK(pad.analog == dal::PadAnalog{});
  }

  dal::GamepadReading all;
  all.buttons = gamepadButtonBit(GamepadButton::A) | gamepadButtonBit(GamepadButton::B) |
                gamepadButtonBit(GamepadButton::DPadUp);
  CHECK(dal::mapGamepadReading(all).buttons ==
        (dal::padButtonBit(dal::PadButton::Cross) | dal::padButtonBit(dal::PadButton::Circle) |
         dal::padButtonBit(dal::PadButton::Up)));
}

void testMapIdleReading() {
  CHECK(dal::mapGamepadReading(dal::GamepadReading{}) == dal::MappedPad{});

  // Noise inside the deadzones and below the trigger threshold is still idle.
  dal::GamepadReading noisy;
  noisy.left_x = 0.1f;
  noisy.left_y = -0.15f;
  noisy.right_x = -0.15f;
  noisy.right_y = 0.15f;
  noisy.left_trigger = 29.0f / 255.0f;
  noisy.right_trigger = 0.05f;
  CHECK(dal::mapGamepadReading(noisy) == dal::MappedPad{});
}

void testStickByte() {
  CHECK(dal::stickByte(0.0f) == 0x80);
  CHECK(dal::stickByte(-0.0f) == 0x80);
  CHECK(dal::stickByte(1.0f) == 0xFF);
  CHECK(dal::stickByte(-1.0f) == 0x00);
  // Out-of-range input is clamped.
  CHECK(dal::stickByte(1.5f) == 0xFF);
  CHECK(dal::stickByte(-1.5f) == 0x00);
  CHECK(dal::stickByte(0.5f) == 191);   // lround(1.5 * 127.5) = 191
  CHECK(dal::stickByte(-0.5f) == 64);   // lround(0.5 * 127.5) = 64
}

void testMapSticks() {
  dal::GamepadReading reading;
  reading.left_x = 1.0f;
  CHECK(dal::mapGamepadReading(reading).analog.left_x == 0xFF);
  reading.left_x = -1.0f;
  CHECK(dal::mapGamepadReading(reading).analog.left_x == 0x00);

  // GameInput +y is up and CellPadData 0 is up.
  reading = {};
  reading.left_y = 1.0f;
  reading.right_y = -1.0f;
  dal::MappedPad pad = dal::mapGamepadReading(reading);
  CHECK(pad.analog.left_y == 0x00);
  CHECK(pad.analog.right_y == 0xFF);
  CHECK(pad.analog.left_x == 0x80 && pad.analog.right_x == 0x80);

  // Each stick has its own deadzone.
  reading = {};
  reading.left_x = 0.23f;   // below 0.2395
  reading.right_x = 0.25f;  // below 0.2652
  CHECK(dal::mapGamepadReading(reading) == dal::MappedPad{});
  reading.left_x = 0.25f;   // above the left deadzone
  reading.right_x = 0.28f;  // above the right deadzone
  pad = dal::mapGamepadReading(reading);
  CHECK(pad.analog.left_x != 0x80);
  CHECK(pad.analog.right_x != 0x80);

  // Just outside the deadzone the output is near the centre: the rescaling avoids a jump.
  reading = {};
  reading.left_x = dal::kLeftStickDeadzone + 0.001f;
  reading.right_y = -(dal::kRightStickDeadzone + 0.001f);
  pad = dal::mapGamepadReading(reading);
  CHECK(std::abs(static_cast<int>(pad.analog.left_x) - 0x80) <= 2);
  CHECK(pad.analog.left_x != 0x00);
  CHECK(std::abs(static_cast<int>(pad.analog.right_y) - 0x80) <= 2);

  // The deadzone is radial: each axis alone is inside it, but the diagonal (radius 0.28) is outside,
  // while a diagonal of radius 0.21 is idle.
  const auto [zx, zy] = dal::applyRadialDeadzone(0.15f, 0.15f, dal::kLeftStickDeadzone);
  CHECK(zx == 0.0f && zy == 0.0f);
  const auto [ox, oy] = dal::applyRadialDeadzone(0.2f, 0.2f, dal::kLeftStickDeadzone);
  CHECK(ox > 0.0f && oy > 0.0f);
  const auto [ex, ey] = dal::applyRadialDeadzone(0.0f, 0.0f, dal::kLeftStickDeadzone);
  CHECK(ex == 0.0f && ey == 0.0f);

  // Direction is kept and a full deflection stays 1.
  const auto [fx, fy] = dal::applyRadialDeadzone(0.0f, -1.0f, dal::kRightStickDeadzone);
  CHECK(fx == 0.0f && std::abs(fy + 1.0f) < 1e-6f);
}

void testMapDiagonalClamped() {
  // (1, 1) has radius sqrt(2); it is clamped to radius 1, not to the square's corner.
  const auto [x, y] = dal::applyRadialDeadzone(1.0f, 1.0f, dal::kLeftStickDeadzone);
  CHECK(std::abs(x - y) < 1e-6f);
  CHECK(std::abs(x - std::sqrt(0.5f)) < 1e-5f);
  const int expected = dal::stickByte(std::sqrt(0.5f));
  CHECK(dal::stickByte(x) == dal::stickByte(y));
  CHECK(std::abs(static_cast<int>(dal::stickByte(x)) - expected) <= 1);

  dal::GamepadReading reading;
  reading.left_x = 1.0f;
  reading.left_y = 1.0f;
  const dal::MappedPad pad = dal::mapGamepadReading(reading);
  CHECK(std::abs(static_cast<int>(pad.analog.left_x) - expected) <= 1);
  // The y byte mirrors the x byte around the centre because y is inverted.
  CHECK(std::abs(static_cast<int>(pad.analog.left_x) + static_cast<int>(pad.analog.left_y) - 255) <= 1);
}

void testMapTriggers() {
  dal::GamepadReading reading;
  reading.left_trigger = 29.0f / 255.0f;
  reading.right_trigger = 29.0f / 255.0f;
  dal::MappedPad pad = dal::mapGamepadReading(reading);
  CHECK(pad.buttons == 0);
  CHECK(pad.analog.l2 == 0 && pad.analog.r2 == 0);

  reading.left_trigger = 30.0f / 255.0f;
  pad = dal::mapGamepadReading(reading);
  CHECK(pad.buttons == dal::padButtonBit(dal::PadButton::L2));
  CHECK(pad.analog.l2 == 30);
  CHECK(pad.analog.r2 == 0);

  reading.left_trigger = 1.0f;
  reading.right_trigger = 1.0f;
  pad = dal::mapGamepadReading(reading);
  CHECK(pad.buttons == (dal::padButtonBit(dal::PadButton::L2) | dal::padButtonBit(dal::PadButton::R2)));
  CHECK(pad.analog.l2 == 255 && pad.analog.r2 == 255);

  // The right trigger is R2 on its own.
  reading = {};
  reading.right_trigger = 0.5f;
  pad = dal::mapGamepadReading(reading);
  CHECK(pad.buttons == dal::padButtonBit(dal::PadButton::R2));
  CHECK(pad.analog.r2 == 128);  // lround(127.5)
}

void testMergeMappedPads() {
  CHECK(dal::mergeMappedPads({}) == dal::MappedPad{});

  dal::MappedPad first;
  first.buttons = dal::padButtonBit(dal::PadButton::Cross);
  first.analog.left_x = 0x90;  // 16 right of centre
  first.analog.left_y = 0x10;  // 112 up
  first.analog.right_x = 0x80;
  first.analog.l2 = 40;
  dal::MappedPad second;
  second.buttons = dal::padButtonBit(dal::PadButton::Circle);
  second.analog.left_x = 0x10;  // 112 left: farther than the first
  second.analog.left_y = 0xA0;  // 32 down: nearer than the first
  second.analog.right_x = 0xF0;
  second.analog.l2 = 200;
  second.analog.r2 = 7;

  const dal::MappedPad pads[] = {first, second};
  const dal::MappedPad merged = dal::mergeMappedPads(pads);
  CHECK(merged.buttons == (dal::padButtonBit(dal::PadButton::Cross) | dal::padButtonBit(dal::PadButton::Circle)));
  CHECK(merged.analog.left_x == 0x10);
  CHECK(merged.analog.left_y == 0x10);
  CHECK(merged.analog.right_x == 0xF0);
  CHECK(merged.analog.right_y == 0x80);
  CHECK(merged.analog.l2 == 200);
  CHECK(merged.analog.r2 == 7);

  // One pad merges to itself.
  const dal::MappedPad one[] = {second};
  CHECK(dal::mergeMappedPads(one) == second);

  // Equal distance on opposite sides: the first pad wins.
  dal::MappedPad left, right;
  left.analog.left_x = 0x40;   // 64 left
  right.analog.left_x = 0xC0;  // 64 right
  const dal::MappedPad tie[] = {left, right};
  CHECK(dal::mergeMappedPads(tie).analog.left_x == 0x40);
  const dal::MappedPad tie_swapped[] = {right, left};
  CHECK(dal::mergeMappedPads(tie_swapped).analog.left_x == 0xC0);
}

void testRumbleFromCellPad() {
  CHECK(dal::rumbleFromCellPad(0, 0) == dal::GamepadRumble{});
  const dal::GamepadRumble small = dal::rumbleFromCellPad(1, 0);
  CHECK(small.high_frequency == 1.0f && small.low_frequency == 0.0f);
  const dal::GamepadRumble full_small = dal::rumbleFromCellPad(255, 0);
  CHECK(full_small.high_frequency == 1.0f);
  const dal::GamepadRumble large = dal::rumbleFromCellPad(0, 255);
  CHECK(large.low_frequency == 1.0f && large.high_frequency == 0.0f);
  const dal::GamepadRumble half = dal::rumbleFromCellPad(0, 128);
  CHECK(half.low_frequency == 128.0f / 255.0f && half.high_frequency == 0.0f);
  const dal::GamepadRumble both = dal::rumbleFromCellPad(3, 255);
  CHECK(both.low_frequency == 1.0f && both.high_frequency == 1.0f);
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
  testMapButtonTable();
  testMapIdleReading();
  testStickByte();
  testMapSticks();
  testMapDiagonalClamped();
  testMapTriggers();
  testMergeMappedPads();
  testRumbleFromCellPad();
  testProbeThreadStartLines();

  std::printf("input core tests: %d passed, %d failed\n", g_passed, g_failed);
  std::fflush(stdout);
  // Detached probe threads still run; leave without static destruction, as the game does.
  std::_Exit(g_failed == 0 ? 0 : 1);
}
