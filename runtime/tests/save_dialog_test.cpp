// Self-checking tests for the save data dialog. The model and text tests inject time, so nothing sleeps; the
// interactive driver tests run the driver on a host thread that holds the guest lock, with a keyboard pad
// provider and no window or game data.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/threads.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "hle/save_dialog.h"
#include "hle/save_dialog_model.h"
#include "hle/save_dialog_text.h"
#include "input/keyboard_pad.h"
#include "input/pad_buttons.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

using dal::DialogAction;
using dal::DialogAnswer;
using dal::DialogInput;
using dal::DialogKind;
using dal::PadButton;
using dal::PadSample;
using dal::SaveDialogModel;
using Clock = std::chrono::steady_clock;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::seconds(100);

Clock::time_point at(int ms) { return kT0 + std::chrono::milliseconds(ms); }

PadSample held(PadButton b) {
    PadSample s;
    s.buttons = dal::padButtonBit(b);
    return s;
}

using Actions = std::vector<DialogAction>;

// A DialogInput that has already seen a neutral sample, so every input counts as released.
DialogInput releasedInput() {
    DialogInput input(dal::dialogButtons());
    input.update(PadSample{}, at(0));
    return input;
}

void testHeldAtConstruction() {
    DialogInput input(dal::dialogButtons());
    const PadSample circle = held(PadButton::Circle);
    CHECK(input.update(circle, at(0)).empty());
    CHECK(input.update(circle, at(50)).empty());
    CHECK(input.update(PadSample{}, at(100)).empty());
    CHECK((input.update(circle, at(150)) == Actions{DialogAction::Accept}));

    // A direction held since construction never repeats either.
    DialogInput dir(dal::dialogButtons());
    const PadSample up = held(PadButton::Up);
    bool acted = false;
    for (int ms = 0; ms <= 3000; ms += 5) acted = acted || !dir.update(up, at(ms)).empty();
    CHECK(!acted);
    dir.update(PadSample{}, at(3005));
    CHECK((dir.update(up, at(3010)) == Actions{DialogAction::Up}));
}

void testDpadRepeat() {
    DialogInput input = releasedInput();
    const PadSample down = held(PadButton::Down);
    CHECK((input.update(down, at(10)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(100)).empty());
    CHECK(input.update(down, at(509)).empty());
    CHECK((input.update(down, at(510)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(709)).empty());
    CHECK((input.update(down, at(710)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(909)).empty());
    CHECK((input.update(down, at(910)) == Actions{DialogAction::Down}));

    // Releasing restarts the delay.
    input.update(PadSample{}, at(950));
    CHECK((input.update(down, at(960)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(1400)).empty());
    CHECK((input.update(down, at(1460)) == Actions{DialogAction::Down}));
}

void testRepeatAfterStall() {
    DialogInput input = releasedInput();
    const PadSample up = held(PadButton::Up);
    CHECK((input.update(up, at(0)) == Actions{DialogAction::Up}));
    // One late sample gives one action, not a burst of the missed repeats.
    CHECK((input.update(up, at(2000)) == Actions{DialogAction::Up}));
    CHECK(input.update(up, at(2005)).empty());
    CHECK((input.update(up, at(2200)) == Actions{DialogAction::Up}));
}

void testStickThresholds() {
    for (const bool left : {true, false}) {
        const auto sample = [left](std::uint8_t y) {
            PadSample s;
            (left ? s.left_y : s.right_y) = y;
            return s;
        };
        DialogInput input = releasedInput();
        CHECK(input.update(sample(30), at(10)).empty());
        CHECK(input.update(sample(225), at(20)).empty());
        CHECK((input.update(sample(29), at(30)) == Actions{DialogAction::Up}));
        input.update(sample(0x80), at(40));
        CHECK((input.update(sample(226), at(50)) == Actions{DialogAction::Down}));
        // Sticks repeat like the d-pad.
        CHECK(input.update(sample(226), at(549)).empty());
        CHECK((input.update(sample(226), at(550)) == Actions{DialogAction::Down}));
        CHECK(input.update(sample(226), at(749)).empty());
        CHECK((input.update(sample(226), at(750)) == Actions{DialogAction::Down}));
    }
}

void testShoulderButtonsNoRepeat() {
    DialogInput input = releasedInput();
    CHECK((input.update(held(PadButton::L1), at(10)) == Actions{DialogAction::PageUp}));
    CHECK((input.update(held(PadButton::R1), at(20)) == Actions{DialogAction::PageDown}));
    bool acted = false;
    for (int ms = 30; ms <= 3000; ms += 5) acted = acted || !input.update(held(PadButton::R1), at(ms)).empty();
    CHECK(!acted);
}

void testOtherButtonsIgnored() {
    DialogInput input = releasedInput();
    for (const PadButton b : {PadButton::Triangle, PadButton::Square, PadButton::Start, PadButton::Select,
                              PadButton::L2, PadButton::R2, PadButton::Left, PadButton::Right}) {
        CHECK(input.update(held(b), at(10)).empty());
        input.update(PadSample{}, at(20));
    }
}

void testEnterButtonAssign() {
    // Circle is enter by default.
    {
        DialogInput input(dal::dialogButtons());
        input.update(PadSample{}, at(0));
        CHECK((input.update(held(PadButton::Circle), at(10)) == Actions{DialogAction::Accept}));
        input.update(PadSample{}, at(20));
        CHECK((input.update(held(PadButton::Cross), at(30)) == Actions{DialogAction::Back}));
    }
    dal::setEnterButtonAssign(dal::EnterButtonAssign::Cross);
    {
        DialogInput input(dal::dialogButtons());
        input.update(PadSample{}, at(0));
        CHECK((input.update(held(PadButton::Cross), at(10)) == Actions{DialogAction::Accept}));
        input.update(PadSample{}, at(20));
        CHECK((input.update(held(PadButton::Circle), at(30)) == Actions{DialogAction::Back}));
    }
    dal::setEnterButtonAssign(dal::EnterButtonAssign::Circle);
}

void testPadSampleFromState() {
    dal::PadPortState state = dal::padStateFromButtons(
            dal::padButtonBit(PadButton::Up) | dal::padButtonBit(PadButton::Circle) | dal::padButtonBit(PadButton::L1));
    state.button[dal::kPadButtonAnalogLeftY] = 12;
    state.button[dal::kPadButtonAnalogRightY] = 250;
    const PadSample sample = dal::padSampleFromState(state);
    CHECK(sample.buttons ==
          (dal::padButtonBit(PadButton::Up) | dal::padButtonBit(PadButton::Circle) | dal::padButtonBit(PadButton::L1)));
    CHECK(sample.left_y == 12);
    CHECK(sample.right_y == 250);
    const PadSample neutral = dal::padSampleFromState(dal::neutralPadState());
    CHECK(neutral.buttons == 0 && neutral.left_y == 0x80 && neutral.right_y == 0x80);
}

std::optional<dal::DialogResult> stepOne(SaveDialogModel& model, DialogAction action, int ms) {
    const DialogAction actions[] = {action};
    return model.step(actions, at(ms));
}

std::optional<dal::DialogResult> idle(SaveDialogModel& model, int ms) { return model.step({}, at(ms)); }

void testPageJumpsClamped() {
    SaveDialogModel model(DialogKind::List, 25, 12, at(0));
    stepOne(model, DialogAction::PageDown, 200);
    CHECK(model.selectedRow() == 22);
    stepOne(model, DialogAction::PageDown, 210);
    CHECK(model.selectedRow() == 24);
    stepOne(model, DialogAction::PageUp, 220);
    CHECK(model.selectedRow() == 14);
    stepOne(model, DialogAction::PageUp, 230);
    CHECK(model.selectedRow() == 4);
    stepOne(model, DialogAction::PageUp, 240);
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Up, 250);
    CHECK(model.selectedRow() == 0);
}

void testNoWrap() {
    SaveDialogModel model(DialogKind::List, 3, 2, at(0));
    stepOne(model, DialogAction::Down, 200);
    CHECK(model.selectedRow() == 2);
    stepOne(model, DialogAction::Up, 210);
    stepOne(model, DialogAction::Up, 220);
    stepOne(model, DialogAction::Up, 230);
    CHECK(model.selectedRow() == 0);
}

void testFadeInIgnoresInput() {
    SaveDialogModel model(DialogKind::List, 5, 0, at(0));
    stepOne(model, DialogAction::Down, 0);
    stepOne(model, DialogAction::Down, 149);
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Down, 150);
    CHECK(model.selectedRow() == 1);

    SaveDialogModel accept(DialogKind::List, 5, 3, at(0));
    stepOne(accept, DialogAction::Accept, 149);
    CHECK(!idle(accept, 400));  // never closed: the Accept at 149 was dropped
    CHECK(accept.opacity(at(400)) == 1.0f);
}

void testResultAfterFadeOut() {
    SaveDialogModel model(DialogKind::List, 5, 3, at(0));
    CHECK(!stepOne(model, DialogAction::Accept, 200));
    CHECK(!idle(model, 349));
    const auto result = idle(model, 350);
    CHECK(result.has_value());
    if (result) {
        CHECK(result->answer == DialogAnswer::Row);
        CHECK(result->row == 3);
    }

    // Input after the close does not change the result.
    SaveDialogModel closing(DialogKind::List, 5, 3, at(0));
    stepOne(closing, DialogAction::Back, 200);
    stepOne(closing, DialogAction::Accept, 250);
    const auto back = idle(closing, 400);
    CHECK(back.has_value() && back->answer == DialogAnswer::Back && back->row == -1);
}

void testFirstCloseWinsWithinOneStep() {
    SaveDialogModel model(DialogKind::List, 5, 1, at(0));
    const DialogAction actions[] = {DialogAction::Back, DialogAction::Accept};
    model.step(actions, at(200));
    const auto result = idle(model, 350);
    CHECK(result.has_value() && result->answer == DialogAnswer::Back);
}

void testEmptyListIgnoresAccept() {
    SaveDialogModel model(DialogKind::List, 0, 4, at(0));
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Accept, 200);
    CHECK(!idle(model, 500));
    CHECK(model.opacity(at(500)) == 1.0f);
    stepOne(model, DialogAction::Down, 510);
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Back, 520);
    const auto result = idle(model, 700);
    CHECK(result.has_value() && result->answer == DialogAnswer::Back);
}

void testOkIgnoresBack() {
    SaveDialogModel model(DialogKind::Ok, 0, 0, at(0));
    stepOne(model, DialogAction::Back, 200);
    CHECK(!idle(model, 500));
    stepOne(model, DialogAction::Accept, 510);
    CHECK(!idle(model, 600));
    const auto result = idle(model, 660);
    CHECK(result.has_value() && result->answer == DialogAnswer::Ok);
}

void testYesNo() {
    SaveDialogModel yes(DialogKind::YesNo, 0, 0, at(0));
    stepOne(yes, DialogAction::Accept, 200);
    const auto y = idle(yes, 350);
    CHECK(y.has_value() && y->answer == DialogAnswer::Yes);

    SaveDialogModel no(DialogKind::YesNo, 0, 0, at(0));
    stepOne(no, DialogAction::Back, 200);
    const auto n = idle(no, 350);
    CHECK(n.has_value() && n->answer == DialogAnswer::No);
}

void testInitialRowAndScroll() {
    // Centred on open, clamped at both ends.
    CHECK(SaveDialogModel(DialogKind::List, 20, 0, at(0)).firstVisibleRow() == 0);
    CHECK(SaveDialogModel(DialogKind::List, 20, 2, at(0)).firstVisibleRow() == 0);
    CHECK(SaveDialogModel(DialogKind::List, 20, 10, at(0)).firstVisibleRow() == 8);
    CHECK(SaveDialogModel(DialogKind::List, 20, 19, at(0)).firstVisibleRow() == 16);
    CHECK(SaveDialogModel(DialogKind::List, 3, 1, at(0)).firstVisibleRow() == 0);
    // initial_row is clamped into the list.
    CHECK(SaveDialogModel(DialogKind::List, 4, 99, at(0)).selectedRow() == 3);
    CHECK(SaveDialogModel(DialogKind::List, 4, -5, at(0)).selectedRow() == 0);
}

void testScrollKeepsSelectionVisible() {
    SaveDialogModel model(DialogKind::List, 12, 0, at(0));
    int ms = 200;
    for (int i = 0; i < 14; i++, ms += 10) {
        stepOne(model, DialogAction::Down, ms);
        CHECK(model.selectedRow() >= model.firstVisibleRow());
        CHECK(model.selectedRow() < model.firstVisibleRow() + SaveDialogModel::kVisibleRows);
    }
    CHECK(model.selectedRow() == 11);
    CHECK(model.firstVisibleRow() == 8);
    // Moving up inside the window does not scroll; leaving it scrolls by the minimum.
    for (int i = 0; i < 4; i++, ms += 10) stepOne(model, DialogAction::Up, ms);
    CHECK(model.selectedRow() == 7);
    CHECK(model.firstVisibleRow() == 7);
    stepOne(model, DialogAction::Up, ms);
    CHECK(model.selectedRow() == 6);
    CHECK(model.firstVisibleRow() == 6);
    ms += 10;
    stepOne(model, DialogAction::PageUp, ms);
    CHECK(model.selectedRow() == 0);
    CHECK(model.firstVisibleRow() == 0);
    ms += 10;
    stepOne(model, DialogAction::PageDown, ms);
    CHECK(model.selectedRow() == 10);
    CHECK(model.firstVisibleRow() == 7);
}

void testOpacity() {
    SaveDialogModel model(DialogKind::List, 3, 0, at(0));
    CHECK(model.opacity(at(0)) == 0.0f);
    CHECK(model.opacity(at(75)) > 0.49f && model.opacity(at(75)) < 0.51f);
    CHECK(model.opacity(at(150)) == 1.0f);
    CHECK(model.opacity(at(1000)) == 1.0f);
    stepOne(model, DialogAction::Back, 1000);
    CHECK(model.opacity(at(1000)) == 1.0f);
    CHECK(model.opacity(at(1075)) > 0.49f && model.opacity(at(1075)) < 0.51f);
    CHECK(model.opacity(at(1150)) == 0.0f);
    CHECK(model.opacity(at(2000)) == 0.0f);
}

void testFormatSaveSize() {
    CHECK(dal::formatSaveSize(0) == "0 KB");
    CHECK(dal::formatSaveSize(1) == "1 KB");
    CHECK(dal::formatSaveSize(1000) == "1 KB");
    CHECK(dal::formatSaveSize(1001) == "2 KB");
    CHECK(dal::formatSaveSize(1000000) == "1000 KB");
    CHECK(dal::formatSaveSize(1000001) == "2 MB");
    CHECK(dal::formatSaveSize(2000000) == "2 MB");
    CHECK(dal::formatSaveSize(2000001) == "3 MB");
}

std::tm fixedTime() {
    std::tm t{};
    t.tm_year = 2015 - 1900;
    t.tm_mon = 7;
    t.tm_mday = 21;
    t.tm_hour = 13;
    t.tm_min = 52;
    return t;
}

void testFormatSaveDate() {
    CHECK(dal::formatSaveDate(fixedTime()) == "2015/08/21  13:52");
    std::tm early{};
    early.tm_year = 2001 - 1900;
    early.tm_mon = 0;
    early.tm_mday = 2;
    early.tm_hour = 3;
    early.tm_min = 4;
    CHECK(dal::formatSaveDate(early) == "2001/01/02  03:04");
    CHECK(dal::saveRowInfo(fixedTime(), 1001) == "2015/08/21  13:52   2 KB");
}

void testSaveConfirmText() {
    CHECK(dal::saveConfirmText("Q?", "T", "S", "D", "Z", "detail") == "Q?\n\nT\nS\nD\nZ\n\ndetail");
    // Empty fields keep their lines.
    CHECK(dal::saveConfirmText("Q?", "T", "", "D", "Z", "") == "Q?\n\nT\n\nD\nZ\n\n");
    CHECK(dal::saveConfirmText(dal::kConfirmOverwrite, "", "", "", "", "") ==
          std::string(dal::kConfirmOverwrite) + "\n\n\n\n\n\n\n");
}

void testSaveErrorText() {
    const auto no_space = dal::saveErrorText(-1, 12, "");
    CHECK(no_space.has_value() && *no_space == "空き容量が足りません。\n\n必要な空き容量：12 KB");
    const auto failure = dal::saveErrorText(-2, 0, "");
    CHECK(failure.has_value() && *failure == "セーブまたはロードに失敗しました。");
    const auto broken = dal::saveErrorText(-3, 0, "");
    CHECK(broken.has_value() && *broken == "セーブデータが壊れています。");
    const auto no_data = dal::saveErrorText(-4, 0, "");
    CHECK(no_data.has_value() && *no_data == "セーブデータが見つかりません。");
    const auto invalid = dal::saveErrorText(-5, 0, "custom message");
    CHECK(invalid.has_value() && *invalid == "custom message");
    const auto invalid_empty = dal::saveErrorText(-5, 0, "");
    CHECK(invalid_empty.has_value() && invalid_empty->empty());
    CHECK(!dal::saveErrorText(3, 0, "ignored").has_value());
    CHECK(!dal::saveErrorText(0, 0, "ignored").has_value());
}

std::shared_ptr<dal::SaveDialogContent> contentWithRows(int rows, DialogKind kind = DialogKind::List) {
    auto content = std::make_shared<dal::SaveDialogContent>();
    content->kind = kind;
    content->rows.resize(static_cast<std::size_t>(rows));
    return content;
}

dal::SaveListRequest listRequest(int rows, std::int32_t initial_row, std::optional<std::int32_t> auto_row) {
    dal::SaveListRequest request;
    request.content = contentWithRows(rows);
    request.initial_row = initial_row;
    request.auto_row = auto_row;
    return request;
}

// Collects the log lines written while it is alive.
class LogCapture {
public:
    LogCapture() {
        dal::setLogSink([this](std::string_view line) { lines.emplace_back(line); });
    }
    ~LogCapture() { dal::setLogSink({}); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::vector<std::string> lines;
};

bool anyLineContains(const LogCapture& log, std::string_view text) {
    for (const auto& line : log.lines) {
        if (line.find(text) != std::string::npos) return true;
    }
    return false;
}

void testAutoDriver() {
    LogCapture log;
    auto driver = dal::makeAutoSaveDialogDriver(dal::SaveDialogPolicy::Auto);
    CHECK(driver->kind() == dal::SaveDialogDriverKind::Auto);
    CHECK(!driver->interactive());

    // The headless choice is passed through; initial_row plays no part.
    CHECK(driver->showList(listRequest(3, 0, 1)) == std::optional<std::int32_t>(1));
    CHECK(driver->showList(listRequest(3, 2, 0)) == std::optional<std::int32_t>(0));
    CHECK(driver->showList(listRequest(3, 1, 2)) == std::optional<std::int32_t>(2));
    CHECK(!driver->showList(listRequest(3, 1, std::nullopt)).has_value());
    CHECK(!driver->showList(listRequest(3, 1, 3)).has_value());
    CHECK(!driver->showList(listRequest(3, 1, -1)).has_value());
    CHECK(!driver->showList(listRequest(0, 0, 0)).has_value());
    dal::SaveListRequest no_content;
    no_content.auto_row = 0;
    CHECK(!driver->showList(no_content).has_value());

    CHECK(driver->showMessage(contentWithRows(0, DialogKind::YesNo)) == dal::SaveMessageResult::Yes);
    CHECK(driver->showMessage(contentWithRows(0, DialogKind::Ok)) == dal::SaveMessageResult::Ok);

    // Existing headless logs must not change.
    CHECK(log.lines.empty());
}

void testCancelDriver() {
    LogCapture log;
    auto driver = dal::makeAutoSaveDialogDriver(dal::SaveDialogPolicy::Cancel);
    CHECK(driver->kind() == dal::SaveDialogDriverKind::Cancel);
    CHECK(!driver->interactive());
    CHECK(!driver->showList(listRequest(3, 1, 1)).has_value());
    CHECK(log.lines.size() == 1 &&
          log.lines[0].find("dal: savedata: dialog (cancel): list of 3 rows -> back") != std::string::npos);
    CHECK(!driver->showList(listRequest(0, 0, std::nullopt)).has_value());
    CHECK(anyLineContains(log, "dialog (cancel): list of 0 rows -> back"));
    CHECK(driver->showMessage(contentWithRows(0, DialogKind::YesNo)) == dal::SaveMessageResult::No);
    CHECK(anyLineContains(log, "dal: savedata: dialog (cancel): confirmation -> no"));
    CHECK(driver->showMessage(contentWithRows(0, DialogKind::Ok)) == dal::SaveMessageResult::Ok);
    CHECK(anyLineContains(log, "dal: savedata: dialog (cancel): message -> ok"));
    CHECK(log.lines.size() == 4);
}

void testBoard() {
    dal::SaveDialogBoard board;
    CHECK(board.read().content == nullptr);
    CHECK(board.drawer() == dal::SaveDialogDrawer::None);

    auto content = contentWithRows(2);
    dal::SaveDialogSnapshot snapshot;
    snapshot.content = content;
    snapshot.selected_row = 1;
    snapshot.first_visible_row = 1;
    snapshot.opacity = 0.5f;
    board.publish(snapshot);
    const auto first = board.read();
    CHECK(first.content == content);
    CHECK(first.selected_row == 1);
    CHECK(first.first_visible_row == 1);
    CHECK(first.opacity == 0.5f);

    board.publish(snapshot);
    const auto second = board.read();
    CHECK(second.serial > first.serial);

    board.clear();
    const auto cleared = board.read();
    CHECK(cleared.content == nullptr);
    CHECK(cleared.serial > second.serial);

    board.setDrawer(dal::SaveDialogDrawer::Ready);
    CHECK(board.drawer() == dal::SaveDialogDrawer::Ready);
    board.setDrawer(dal::SaveDialogDrawer::Failed);
    CHECK(board.drawer() == dal::SaveDialogDrawer::Failed);
    board.clear();
    CHECK(board.drawer() == dal::SaveDialogDrawer::Failed);
}

void testDriverRegistry() {
    LogCapture log;
    dal::resetSaveDialog();
    CHECK(dal::saveDialogDriver().kind() == dal::SaveDialogDriverKind::Auto);
    CHECK(dal::saveDialogDriver().showMessage(contentWithRows(0, DialogKind::YesNo)) == dal::SaveMessageResult::Yes);
    CHECK(!dal::saveDialogOpen());

    dal::setSaveDialogDriver(dal::makeAutoSaveDialogDriver(dal::SaveDialogPolicy::Cancel));
    CHECK(dal::saveDialogDriver().kind() == dal::SaveDialogDriverKind::Cancel);
    CHECK(dal::saveDialogDriver().showMessage(contentWithRows(0, DialogKind::YesNo)) == dal::SaveMessageResult::No);
    dal::setSaveDialogDriver(nullptr);
    CHECK(dal::saveDialogDriver().kind() == dal::SaveDialogDriverKind::Auto);
    CHECK(dal::saveDialogDriver().showMessage(contentWithRows(0, DialogKind::YesNo)) == dal::SaveMessageResult::Yes);

    // resetSaveDialog empties the board and the drawer too.
    dal::saveDialogBoard().setDrawer(dal::SaveDialogDrawer::Ready);
    dal::SaveDialogSnapshot snapshot;
    snapshot.content = contentWithRows(1);
    dal::saveDialogBoard().publish(snapshot);
    dal::setSaveDialogDriver(dal::makeAutoSaveDialogDriver(dal::SaveDialogPolicy::Cancel));
    dal::resetSaveDialog();
    CHECK(dal::saveDialogDriver().kind() == dal::SaveDialogDriverKind::Auto);
    CHECK(dal::saveDialogBoard().read().content == nullptr);
    CHECK(dal::saveDialogBoard().drawer() == dal::SaveDialogDrawer::None);
}

// Every wait below has a deadline. When one expires the dialog is told its overlay failed, which makes the driver
// leave its loop, so a broken driver fails checks instead of hanging the run.
constexpr std::chrono::milliseconds kDeadline{5000};

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout = kDeadline) {
    const auto deadline = Clock::now() + timeout;
    while (!pred()) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// A host thread that holds the guest lock while it runs `body`, like a guest thread inside a handler.
class GuestRun {
public:
    GuestRun(dal::SaveDialogBoard& board, std::function<void()> body) : board_(board) {
        thread_ = std::thread([this, body = std::move(body)] {
            dal::guestLock().lock();
            body();
            dal::guestLock().unlock();
            finished_at_ = Clock::now();
            done_.store(true);
        });
    }
    ~GuestRun() { join(); }
    GuestRun(const GuestRun&) = delete;
    GuestRun& operator=(const GuestRun&) = delete;

    // False when the body did not end within `timeout`.
    bool join(std::chrono::milliseconds timeout = kDeadline) {
        const bool finished = waitFor([this] { return done_.load(); }, timeout);
        if (!finished) board_.setDrawer(dal::SaveDialogDrawer::Failed);
        if (thread_.joinable()) thread_.join();
        return finished;
    }

    // When the body ended; valid after join() returned true.
    Clock::time_point finishedAt() const { return finished_at_; }

private:
    dal::SaveDialogBoard& board_;
    Clock::time_point finished_at_{};
    std::atomic<bool> done_{false};
    std::thread thread_;
};

// Runs `fn` on a thread that holds the guest lock, as the test's own view into state that handlers guard. A driver
// that kept the lock during its wait leaves this call waiting until the join deadline, which then fails the check
// (and makes the driver give up) instead of passing. A driver that ignored the drawer as well would still hang the
// join.
bool underGuestLock(dal::SaveDialogBoard& board, std::function<void()> fn) {
    GuestRun run(board, std::move(fn));
    return run.join();
}

struct DialogFixture {
    std::shared_ptr<dal::KeyboardPadState> pad = std::make_shared<dal::KeyboardPadState>();
    dal::SaveDialogBoard board;
    std::unique_ptr<dal::SaveDialogDriver> driver;

    explicit DialogFixture(bool drawer_ready = true) {
        dal::resetCellSysutil();
        dal::resetCellMsgDialog();
        dal::resetCellPad();
        dal::resetSaveDialog();
        dal::setPadProvider(dal::makeKeyboardPadProvider(pad));
        if (drawer_ready) board.setDrawer(dal::SaveDialogDrawer::Ready);
        driver = dal::makeInteractiveSaveDialogDriver(board);
    }
    ~DialogFixture() {
        dal::resetCellMsgDialog();
        dal::setPadProvider(nullptr);
        dal::resetCellPad();
        dal::resetSaveDialog();
        dal::resetCellSysutil();
    }

    void press(PadButton button, bool down) {
        pad->setButtons(dal::InputSource::Keyboard, dal::padButtonBit(button), down, Clock::now());
    }
    bool waitForDialog() {
        return waitFor([this] { return board.read().content != nullptr; });
    }
    // Input is dropped during the fade-in, so presses wait for it to end.
    bool waitForFadeIn() {
        return waitFor([this] {
            const auto snapshot = board.read();
            return snapshot.content && snapshot.opacity >= 0.999f;
        });
    }
};

// What the test thread sees of the system state while no dialog code runs.
struct SystemView {
    bool intercepted = false;
    bool dialog_open = false;
    std::int32_t begin = 0;  // queueSysutilEvent(DRAWING_BEGIN)
};

SystemView viewSystem(DialogFixture& f, bool probe_begin) {
    SystemView view;
    CHECK(underGuestLock(f.board, [&] {
        view.intercepted = dal::padIntercepted();
        view.dialog_open = dal::saveDialogOpen();
        if (probe_begin) view.begin = dal::queueSysutilEvent(dal::kSysutilDrawingBegin, 0);
    }));
    return view;
}

// DRAWING_END for a BEGIN the test sent itself or probed successfully.
std::int32_t sendEnd(DialogFixture& f) {
    std::int32_t result = 0;
    CHECK(underGuestLock(f.board, [&] { result = dal::queueSysutilEvent(dal::kSysutilDrawingEnd, 0); }));
    return result;
}

constexpr auto kBusy = static_cast<std::int32_t>(dal::kSysutilErrorBusy);

// Log lines from any thread; the dialog thread and the test's guest-lock threads can log at the same time.
class SharedLogCapture {
public:
    SharedLogCapture() {
        dal::setLogSink([this](std::string_view line) {
            std::lock_guard lock(mutex_);
            lines_.emplace_back(line);
        });
    }
    ~SharedLogCapture() { dal::setLogSink({}); }
    SharedLogCapture(const SharedLogCapture&) = delete;
    SharedLogCapture& operator=(const SharedLogCapture&) = delete;

    bool contains(std::string_view text) const {
        std::lock_guard lock(mutex_);
        for (const std::string& line : lines_) {
            if (line.find(text) != std::string::npos) return true;
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

// The interactive list: published, pad taken over, pad given back, DRAWING_BEGIN held and released. The dialog
// thread has to give up the guest lock during the wait, or the test's own views would time out.
void testInteractiveList() {
    DialogFixture f;
    std::optional<std::int32_t> row = 99;
    GuestRun run(f.board, [&] { row = f.driver->showList({contentWithRows(3), 1}); });

    CHECK(f.waitForDialog());
    const auto first = f.board.read();
    CHECK(first.content && first.content->kind == DialogKind::List && first.content->id != 0);
    CHECK(first.selected_row == 1);

    // The guest lock is free during the wait: a second BEGIN is refused and the pad is intercepted.
    const SystemView during = viewSystem(f, true);
    CHECK(during.begin == kBusy);
    CHECK(during.intercepted);
    CHECK(during.dialog_open);

    CHECK(f.waitForFadeIn());
    f.press(PadButton::Down, true);
    CHECK(waitFor([&] { return f.board.read().selected_row == 2; }));
    f.press(PadButton::Down, false);
    f.press(PadButton::Circle, true);
    CHECK(run.join());
    f.press(PadButton::Circle, false);

    CHECK(row == std::optional<std::int32_t>(2));
    CHECK(f.board.read().content == nullptr);
    const SystemView after = viewSystem(f, true);
    CHECK(!after.intercepted);
    CHECK(!after.dialog_open);
    CHECK(after.begin == 0);  // the dialog's END went out, so BEGIN is accepted again
    CHECK(sendEnd(f) == 0);
}

void testInteractiveMessage() {
    struct Case {
        DialogKind kind;
        PadButton button;
        dal::SaveMessageResult expected;
    };
    const Case cases[] = {
            {DialogKind::YesNo, PadButton::Circle, dal::SaveMessageResult::Yes},
            {DialogKind::YesNo, PadButton::Cross, dal::SaveMessageResult::No},
            {DialogKind::Ok, PadButton::Circle, dal::SaveMessageResult::Ok},
    };
    for (const Case& c : cases) {
        DialogFixture f;
        dal::SaveMessageResult result = dal::SaveMessageResult::Failed;
        GuestRun run(f.board, [&] { result = f.driver->showMessage(contentWithRows(0, c.kind)); });
        CHECK(f.waitForFadeIn());
        const auto shown = f.board.read();
        CHECK(shown.content && shown.content->kind == c.kind);
        f.press(c.button, true);
        CHECK(run.join());
        f.press(c.button, false);
        CHECK(result == c.expected);
        CHECK(sendEnd(f) == -1);  // END already went out
    }
}

// Without anything to draw the dialog is answered as Cancel at once and touches neither the pad nor DRAWING_BEGIN.
void testNoDrawerAnswersCancel() {
    for (const dal::SaveDialogDrawer drawer : {dal::SaveDialogDrawer::None, dal::SaveDialogDrawer::Failed}) {
        DialogFixture f(false);
        f.board.setDrawer(drawer);
        std::optional<std::int32_t> row = 99;
        dal::SaveMessageResult yes_no = dal::SaveMessageResult::Failed;
        dal::SaveMessageResult ok = dal::SaveMessageResult::Failed;
        SystemView after;
        GuestRun run(f.board, [&] {
            row = f.driver->showList({contentWithRows(3), 1});
            yes_no = f.driver->showMessage(contentWithRows(0, DialogKind::YesNo));
            ok = f.driver->showMessage(contentWithRows(0, DialogKind::Ok));
            after.intercepted = dal::padIntercepted();
            after.dialog_open = dal::saveDialogOpen();
            after.begin = dal::queueSysutilEvent(dal::kSysutilDrawingBegin, 0);
        });
        CHECK(run.join());
        CHECK(!row.has_value());
        CHECK(yes_no == dal::SaveMessageResult::No);
        CHECK(ok == dal::SaveMessageResult::Ok);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(after.begin == 0);
        CHECK(f.board.read().content == nullptr);
    }
}

// The overlay can fail while a dialog waits. The driver must notice within a few ticks, answer as Cancel and still
// release the pad and DRAWING_BEGIN.
void testDrawerFailsDuringWait() {
    for (const DialogKind kind : {DialogKind::List, DialogKind::YesNo}) {
        DialogFixture f;
        std::optional<std::int32_t> row = 99;
        dal::SaveMessageResult message = dal::SaveMessageResult::Failed;
        GuestRun run(f.board, [&] {
            if (kind == DialogKind::List) {
                row = f.driver->showList({contentWithRows(2), 0});
            } else {
                message = f.driver->showMessage(contentWithRows(0, kind));
            }
        });
        CHECK(f.waitForDialog());
        CHECK(viewSystem(f, false).intercepted);
        const auto failed_at = Clock::now();
        f.board.setDrawer(dal::SaveDialogDrawer::Failed);
        const std::uint64_t serial_at_failure = f.board.read().serial;
        CHECK(run.join());
        // The driver notices on its next wake: at most one publish already in flight, then the clear. Counting
        // publishes keeps this independent of machine load; the wall-time bound only guards against a hang.
        CHECK(f.board.read().serial <= serial_at_failure + 2);
        CHECK(run.finishedAt() - failed_at < std::chrono::milliseconds(1000));
        if (kind == DialogKind::List) {
            CHECK(!row.has_value());
        } else {
            CHECK(message == dal::SaveMessageResult::No);
        }
        CHECK(f.board.read().content == nullptr);
        const SystemView after = viewSystem(f, false);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(sendEnd(f) == -1);  // END was sent
    }
}

// Another system dialog holds DRAWING_BEGIN. A message cannot be shown; a list opens anyway and leaves that BEGIN
// alone.
void testBeginOutstanding() {
    {
        DialogFixture f;
        CHECK(underGuestLock(f.board, [&] { CHECK(dal::queueSysutilEvent(dal::kSysutilDrawingBegin, 0) == 0); }));
        dal::SaveMessageResult result = dal::SaveMessageResult::Ok;
        GuestRun run(f.board, [&] { result = f.driver->showMessage(contentWithRows(0, DialogKind::YesNo)); });
        CHECK(run.join());
        CHECK(result == dal::SaveMessageResult::Failed);
        const SystemView after = viewSystem(f, true);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(after.begin == kBusy);  // still the other dialog's
        CHECK(f.board.read().content == nullptr);
        CHECK(sendEnd(f) == 0);
    }
    {
        DialogFixture f;
        CHECK(underGuestLock(f.board, [&] { CHECK(dal::queueSysutilEvent(dal::kSysutilDrawingBegin, 0) == 0); }));
        std::optional<std::int32_t> row = 99;
        GuestRun run(f.board, [&] { row = f.driver->showList({contentWithRows(2), 0}); });
        CHECK(f.waitForFadeIn());
        const SystemView during = viewSystem(f, false);
        CHECK(during.intercepted && during.dialog_open);
        f.press(PadButton::Circle, true);
        CHECK(run.join());
        f.press(PadButton::Circle, false);
        CHECK(row == std::optional<std::int32_t>(0));
        const SystemView after = viewSystem(f, true);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(after.begin == kBusy);  // no END was sent for a BEGIN the dialog did not make
        CHECK(sendEnd(f) == 0);
    }
}

// Calls the game-facing cellMsgDialogOpen2 handler. `type` 0 is a dialog without buttons, which stays open until
// cellMsgDialogClose.
std::uint32_t callMsgDialogOpen2(std::uint32_t type) {
    static std::vector<std::uint8_t> memory(0x400);
    std::memcpy(memory.data() + 0x100, "hello", 6);
    const dal::HleEntry* open2 = nullptr;
    for (const dal::HleEntry& entry : dal::cellMsgDialogHandlers()) {
        if (std::string_view(entry.name) == "cellMsgDialogOpen2") open2 = &entry;
    }
    CHECK(open2 != nullptr);
    if (open2 == nullptr) return 0xffffffff;
    auto ctx = std::make_unique<DalPpuContext>();
    ctx->gpr[3] = type;
    ctx->gpr[4] = 0x100;
    open2->handler(ctx.get(), memory.data());
    return static_cast<std::uint32_t>(ctx->gpr[3]);
}

// The game's own cellMsgDialogOpen2 is refused while a save dialog is open, and accepted once it closed.
void testMsgDialogBusy() {
    DialogFixture f;
    constexpr std::uint32_t kOkButton = 2u << 4;

    std::optional<std::int32_t> row = 99;
    GuestRun run(f.board, [&] { row = f.driver->showList({contentWithRows(1), 0}); });
    CHECK(f.waitForFadeIn());

    // BEGIN is outstanding too, so the result alone cannot tell the two refusals apart; the DRAWING_BEGIN error
    // line only appears when the handler got as far as asking for it.
    SharedLogCapture log;
    std::uint32_t during = 0;
    CHECK(underGuestLock(f.board, [&] { during = callMsgDialogOpen2(kOkButton); }));
    CHECK(during == dal::kSysutilErrorBusy);
    CHECK(!log.contains("second DRAWING_BEGIN"));

    f.press(PadButton::Circle, true);
    CHECK(run.join());
    f.press(PadButton::Circle, false);
    std::uint32_t after = 0xffffffff;
    CHECK(underGuestLock(f.board, [&] { after = callMsgDialogOpen2(kOkButton); }));
    CHECK(after == 0);
}

// The game's message dialog is open: a confirmation and an error message are not shown, a list still opens but
// sends no DRAWING_BEGIN or END of its own (RPCS3 behaviour).
void testGameMsgDialogOpen() {
    {
        DialogFixture f;
        std::uint32_t opened = 0xffffffff;
        bool game_dialog_open = false;
        CHECK(underGuestLock(f.board, [&] {
            opened = callMsgDialogOpen2(0);
            game_dialog_open = dal::msgDialogOpen();
        }));
        CHECK(opened == 0);
        CHECK(game_dialog_open);

        SharedLogCapture log;
        dal::SaveMessageResult yes_no = dal::SaveMessageResult::Ok;
        dal::SaveMessageResult ok = dal::SaveMessageResult::Yes;
        SystemView view;
        GuestRun run(f.board, [&] {
            yes_no = f.driver->showMessage(contentWithRows(0, DialogKind::YesNo));
            ok = f.driver->showMessage(contentWithRows(0, DialogKind::Ok));
            view.intercepted = dal::padIntercepted();
            view.dialog_open = dal::saveDialogOpen();
        });
        CHECK(run.join());
        CHECK(yes_no == dal::SaveMessageResult::Failed);
        CHECK(ok == dal::SaveMessageResult::Failed);
        CHECK(!view.intercepted && !view.dialog_open);
        CHECK(f.board.read().content == nullptr);
        CHECK(log.contains("dal: savedata: confirmation not shown, the game's message dialog is open"));
        CHECK(log.contains("dal: savedata: message not shown, the game's message dialog is open"));
        CHECK(!log.contains("second DRAWING_BEGIN"));
        CHECK(!log.contains("DRAWING_BEGIN refused"));
    }
    {
        DialogFixture f;
        CHECK(underGuestLock(f.board, [&] { CHECK(callMsgDialogOpen2(0) == 0); }));

        SharedLogCapture log;
        std::optional<std::int32_t> row = 99;
        GuestRun run(f.board, [&] { row = f.driver->showList({contentWithRows(2), 1}); });
        CHECK(f.waitForFadeIn());
        const SystemView during = viewSystem(f, false);
        CHECK(during.intercepted && during.dialog_open);
        f.press(PadButton::Circle, true);
        CHECK(run.join());
        f.press(PadButton::Circle, false);
        CHECK(row == std::optional<std::int32_t>(1));
        CHECK(log.contains("dal: savedata: the game's message dialog is open, opening the list without DRAWING_BEGIN"));

        const SystemView after = viewSystem(f, true);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(after.begin == kBusy);  // the game dialog's BEGIN; the list sent no END for it
        CHECK(sendEnd(f) == 0);
    }
}

}  // namespace

int main() {
    testHeldAtConstruction();
    testDpadRepeat();
    testRepeatAfterStall();
    testStickThresholds();
    testShoulderButtonsNoRepeat();
    testOtherButtonsIgnored();
    testEnterButtonAssign();
    testPadSampleFromState();
    testPageJumpsClamped();
    testNoWrap();
    testFadeInIgnoresInput();
    testResultAfterFadeOut();
    testFirstCloseWinsWithinOneStep();
    testEmptyListIgnoresAccept();
    testOkIgnoresBack();
    testYesNo();
    testInitialRowAndScroll();
    testScrollKeepsSelectionVisible();
    testOpacity();
    testFormatSaveSize();
    testFormatSaveDate();
    testSaveConfirmText();
    testSaveErrorText();
    testAutoDriver();
    testCancelDriver();
    testBoard();
    testDriverRegistry();
    testInteractiveList();
    testInteractiveMessage();
    testNoDrawerAnswersCancel();
    testDrawerFailsDuringWait();
    testBeginOutstanding();
    testMsgDialogBusy();
    testGameMsgDialogOpen();
    std::printf("save dialog tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
