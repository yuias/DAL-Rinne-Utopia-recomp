// Self-checking tests for the save data dialog model. Time is injected, so nothing sleeps and no pad provider,
// window or game data is involved.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "hle/save_dialog.h"
#include "hle/save_dialog_model.h"
#include "hle/save_dialog_text.h"
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
    std::printf("save dialog tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
