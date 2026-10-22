#include "hle/save_dialog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <utility>

#include "core/log.h"
#include "core/threads.h"
#include "hle/hle.h"
#include "input/pad_buttons.h"

namespace dal {

namespace {

// Answers without showing or waiting. Auto returns the caller's headless choice and says nothing, so existing
// runs keep their logs; Cancel backs out and logs each answer.
class PolicyDriver final : public SaveDialogDriver {
public:
    explicit PolicyDriver(SaveDialogPolicy policy) : policy_(policy) {}

    SaveDialogDriverKind kind() const override {
        return policy_ == SaveDialogPolicy::Auto ? SaveDialogDriverKind::Auto : SaveDialogDriverKind::Cancel;
    }

    std::optional<std::int32_t> showList(const SaveListRequest& request) override {
        const auto rows = request.content ? static_cast<std::int32_t>(request.content->rows.size()) : 0;
        if (policy_ == SaveDialogPolicy::Auto) {
            if (request.auto_row && *request.auto_row >= 0 && *request.auto_row < rows) return request.auto_row;
            return std::nullopt;
        }
        logInfo("dal: savedata: dialog (cancel): list of {} rows -> back", rows);
        return std::nullopt;
    }

    SaveMessageResult showMessage(std::shared_ptr<SaveDialogContent> content) override {
        const bool confirmation = content && content->kind == DialogKind::YesNo;
        if (policy_ == SaveDialogPolicy::Auto) return confirmation ? SaveMessageResult::Yes : SaveMessageResult::Ok;
        if (confirmation) {
            logInfo("dal: savedata: dialog (cancel): confirmation -> no");
            return SaveMessageResult::No;
        }
        logInfo("dal: savedata: dialog (cancel): message -> ok");
        return SaveMessageResult::Ok;
    }

private:
    SaveDialogPolicy policy_;
};

std::unique_ptr<SaveDialogDriver>& currentDriver() {
    static std::unique_ptr<SaveDialogDriver> driver = makeAutoSaveDialogDriver(SaveDialogPolicy::Auto);
    return driver;
}

std::atomic<bool> g_dialog_open{false};
std::atomic<std::uint64_t> g_next_content_id{1};

const char* kindName(DialogKind kind) {
    switch (kind) {
        case DialogKind::List: return "list";
        case DialogKind::YesNo: return "confirmation";
        case DialogKind::Ok: return "message";
    }
    return "?";
}

const char* answerName(const DialogResult& result) {
    switch (result.answer) {
        case DialogAnswer::Row: return "row";
        case DialogAnswer::Back: return "back";
        case DialogAnswer::Yes: return "yes";
        case DialogAnswer::No: return "no";
        case DialogAnswer::Ok: return "ok";
    }
    return "?";
}

// The state an open interactive dialog holds: the open flag, the pad, the board and (when the dialog sent the BEGIN)
// the DRAWING_END.
class OpenScope {
public:
    OpenScope(SaveDialogBoard& board, bool send_end) : board_(board), send_end_(send_end) {
        g_dialog_open.store(true);
        setPadIntercepted(true);
    }
    ~OpenScope() {
        board_.clear();
        setPadIntercepted(false);
        g_dialog_open.store(false);
        if (send_end_) queueSysutilEvent(kSysutilDrawingEnd, 0);
    }
    OpenScope(const OpenScope&) = delete;
    OpenScope& operator=(const OpenScope&) = delete;

private:
    SaveDialogBoard& board_;
    bool send_end_;
};

// Shows a dialog on the board and waits for the player on the calling guest thread. Every wait is a timed
// blockingWait, which gives up the guest lock so the rest of the game keeps running, and whose timeout keeps the
// thread out of the supervisor's "indefinite wait" census while the player reads.
class InteractiveDriver final : public SaveDialogDriver {
public:
    explicit InteractiveDriver(SaveDialogBoard& board) : board_(board) {}

    SaveDialogDriverKind kind() const override { return SaveDialogDriverKind::Interactive; }

    std::optional<std::int32_t> showList(const SaveListRequest& request) override {
        auto content = request.content ? request.content : std::make_shared<SaveDialogContent>();
        content->kind = DialogKind::List;
        const auto result = run(std::move(content), request.initial_row);
        if (result && result->answer == DialogAnswer::Row) return result->row;
        return std::nullopt;
    }

    SaveMessageResult showMessage(std::shared_ptr<SaveDialogContent> content) override {
        if (!content) content = std::make_shared<SaveDialogContent>();
        const DialogKind kind = content->kind;
        const auto result = run(std::move(content), 0);
        if (!result) return SaveMessageResult::Failed;
        if (kind == DialogKind::YesNo) {
            return result->answer == DialogAnswer::Yes ? SaveMessageResult::Yes : SaveMessageResult::No;
        }
        return SaveMessageResult::Ok;
    }

private:
    static constexpr std::chrono::microseconds kTick{5000};

    // nullopt: a message that could not be shown (the game's message dialog is open, or its DRAWING_BEGIN was
    // refused). Otherwise the player's answer, or the Cancel answer when nobody can draw the dialog.
    std::optional<DialogResult> run(std::shared_ptr<SaveDialogContent> content, std::int32_t initial_row) {
        const DialogKind kind = content->kind;
        const auto rows = static_cast<std::int32_t>(content->rows.size());

        // A dialog nobody can draw must not hang the game.
        if (board_.drawer() != SaveDialogDrawer::Ready) {
            logOnce(LogLevel::Warn, "savedata dialog: no drawer",
                    "dal: savedata: dialog has no drawer (no window or the overlay failed); answering as cancel");
            return cancelAnswer(kind);
        }

        // RPCS3 refuses the confirmation and ignores the error message while the game's message dialog exists
        // (its Open2 returns BUSY), but its save list is a separate overlay that still opens. The game dialog
        // holds DRAWING_BEGIN, so the list sends none.
        const bool game_dialog = msgDialogOpen();
        if (game_dialog) {
            if (kind != DialogKind::List) {
                logInfo("dal: savedata: {} not shown, the game's message dialog is open", kindName(kind));
                return std::nullopt;
            }
            logInfo("dal: savedata: the game's message dialog is open, opening the list without DRAWING_BEGIN");
        }

        // A refused BEGIN (another system dialog holds it) cannot show a message; a list opens anyway and then
        // leaves the other dialog's BEGIN alone (RPCS3 ignores the failure the same way).
        const bool send_end = !game_dialog && queueSysutilEvent(kSysutilDrawingBegin, 0) >= 0;
        if (!send_end && !game_dialog) {
            if (kind != DialogKind::List) {
                logWarn("dal: savedata: {} not shown, DRAWING_BEGIN refused", kindName(kind));
                return std::nullopt;
            }
            logWarn("dal: savedata: DRAWING_BEGIN refused, opening the list anyway");
        }

        // Undoes the open state on every exit, including a throwing pad provider.
        const OpenScope scope(board_, send_end);
        content->id = g_next_content_id.fetch_add(1);
        const std::shared_ptr<const SaveDialogContent> shown = content;
        if (kind == DialogKind::List) {
            logInfo("dal: savedata: dialog opened: list of {} rows", rows);
        } else {
            logInfo("dal: savedata: dialog opened: {}", kindName(kind));
        }

        DialogInput input(dialogButtons());
        SaveDialogModel model(kind, rows, initial_row, SaveDialogModel::Clock::now());
        auto publish = [&](SaveDialogModel::Clock::time_point now) {
            SaveDialogSnapshot snapshot;
            snapshot.content = shown;
            snapshot.selected_row = model.selectedRow();
            snapshot.first_visible_row = model.firstVisibleRow();
            snapshot.opacity = model.opacity(now);
            board_.publish(std::move(snapshot));
        };
        publish(SaveDialogModel::Clock::now());

        std::condition_variable_any tick;  // nobody signals it: the timeout is the tick
        DialogResult result;
        for (;;) {
            blockingWait(tick, [] { return false; }, "savedata dialog", kTick);
            // The overlay can fail after the dialog opened.
            if (board_.drawer() != SaveDialogDrawer::Ready) {
                logOnce(LogLevel::Warn, "savedata dialog: drawer lost",
                        "dal: savedata: the dialog overlay failed while a dialog was open; answering as cancel");
                result = cancelAnswer(kind);
                break;
            }
            // The guest lock is held again here, which the pad provider requires. A port that is not connected
            // reads as zeros, which would look like a stick held up.
            const auto now = SaveDialogModel::Clock::now();
            const PadPortState state = pollHostPad(0);
            const auto actions = input.update(state.connected ? padSampleFromState(state) : PadSample{}, now);
            if (const auto done = model.step(actions, now)) {
                result = *done;
                break;
            }
            publish(now);
        }

        if (result.answer == DialogAnswer::Row) {
            logInfo("dal: savedata: dialog closed: {} -> row {}", kindName(kind), result.row);
        } else {
            logInfo("dal: savedata: dialog closed: {} -> {}", kindName(kind), answerName(result));
        }
        return result;
    }

    // What the Cancel policy answers for `kind`.
    static DialogResult cancelAnswer(DialogKind kind) {
        DialogResult result;
        switch (kind) {
            case DialogKind::List: result.answer = DialogAnswer::Back; break;
            case DialogKind::YesNo: result.answer = DialogAnswer::No; break;
            case DialogKind::Ok: result.answer = DialogAnswer::Ok; break;
        }
        return result;
    }

    SaveDialogBoard& board_;
};

}  // namespace

void SaveDialogBoard::publish(SaveDialogSnapshot snapshot) {
    std::lock_guard lock(mutex_);
    snapshot.serial = ++serial_;
    snapshot_ = std::move(snapshot);
}

void SaveDialogBoard::clear() {
    std::lock_guard lock(mutex_);
    snapshot_ = SaveDialogSnapshot{};
    snapshot_.serial = ++serial_;
}

SaveDialogSnapshot SaveDialogBoard::read() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void SaveDialogBoard::setDrawer(SaveDialogDrawer drawer) {
    std::lock_guard lock(mutex_);
    drawer_ = drawer;
}

SaveDialogDrawer SaveDialogBoard::drawer() const {
    std::lock_guard lock(mutex_);
    return drawer_;
}

SaveDialogBoard& saveDialogBoard() {
    static SaveDialogBoard board;
    return board;
}

std::unique_ptr<SaveDialogDriver> makeAutoSaveDialogDriver(SaveDialogPolicy policy) {
    return std::make_unique<PolicyDriver>(policy);
}

std::unique_ptr<SaveDialogDriver> makeInteractiveSaveDialogDriver(SaveDialogBoard& board) {
    return std::make_unique<InteractiveDriver>(board);
}

void setSaveDialogDriver(std::unique_ptr<SaveDialogDriver> driver) {
    currentDriver() = driver ? std::move(driver) : makeAutoSaveDialogDriver(SaveDialogPolicy::Auto);
}

SaveDialogDriver& saveDialogDriver() { return *currentDriver(); }

bool saveDialogOpen() { return g_dialog_open.load(); }

void resetSaveDialog() {
    setSaveDialogDriver(nullptr);
    g_dialog_open.store(false);
    saveDialogBoard().clear();
    saveDialogBoard().setDrawer(SaveDialogDrawer::None);
}

}  // namespace dal
