#include "hle/save_dialog.h"

#include <atomic>
#include <utility>

#include "core/log.h"

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
