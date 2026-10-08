#include "ReceiptStore.h"

#include "eda-platform/Platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace eda {
namespace agent {
namespace {

std::uint64_t NowEpoch() { return static_cast<std::uint64_t>(std::time(nullptr)); }

std::string FormatEpoch(std::uint64_t epoch) {
    const std::time_t value = static_cast<std::time_t>(epoch);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &value);
#else
    gmtime_r(&value, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

bool IsExpired(const UiReceipt& receipt, std::uint64_t now) {
    return now >= receipt.issuedAtEpoch + receipt.ttlSeconds;
}

Json ReceiptToJson(const UiReceipt& receipt) {
    Json consumed = Json::object();
    if (receipt.status == "consumed") {
        consumed = Json{{"run_id", receipt.consumedByRunId},
                        {"action_id", receipt.consumedActionId},
                        {"state_version", std::to_string(receipt.stateVersion)},
                        {"at_epoch", std::to_string(receipt.consumedAtEpoch)}};
    }
    return Json{{"receipt_id", receipt.id},
                {"project_id", receipt.projectId},
                {"session_id", receipt.sessionId},
                {"issue", receipt.issue},
                {"level", receipt.level},
                {"policy", receipt.policy},
                {"revision", receipt.revision},
                {"challenge_id", receipt.challengeId},
                {"action_id", receipt.actionId},
                {"status", receipt.status},
                {"expires_at", receipt.expiresAt},
                {"issued_at_epoch", std::to_string(receipt.issuedAtEpoch)},
                {"ttl_seconds", std::to_string(receipt.ttlSeconds)},
                {"consumed", consumed}};
}

bool ParseUnsigned(const Json& object, const char* key, std::uint64_t& out) {
    if (!object.contains(key)) return false;
    try {
        if (object[key].is_string()) {
            out = std::stoull(object[key].get<std::string>());
            return true;
        }
        if (object[key].is_number_unsigned()) {
            out = object[key].get<std::uint64_t>();
            return true;
        }
    } catch (const std::exception&) {
    }
    return false;
}

bool ParseReceipt(const Json& value, UiReceipt& out) {
    if (!value.is_object()) return false;
    UiReceipt receipt;
    receipt.id = value.value("receipt_id", std::string());
    receipt.projectId = value.value("project_id", std::string());
    receipt.sessionId = value.value("session_id", std::string());
    receipt.issue = value.value("issue", std::string());
    receipt.level = value.value("level", std::string());
    receipt.policy = value.value("policy", std::string());
    receipt.revision = value.value("revision", std::string());
    receipt.challengeId = value.value("challenge_id", std::string());
    receipt.actionId = value.value("action_id", std::string());
    receipt.status = value.value("status", std::string());
    receipt.expiresAt = value.value("expires_at", std::string());
    if (!ParseUnsigned(value, "issued_at_epoch", receipt.issuedAtEpoch) ||
        !ParseUnsigned(value, "ttl_seconds", receipt.ttlSeconds)) {
        return false;
    }
    if (value.contains("consumed") && value["consumed"].is_object()) {
        const Json& consumed = value["consumed"];
        if (!consumed.empty()) {
            receipt.consumedByRunId = consumed.value("run_id", std::string());
            receipt.consumedActionId = consumed.value("action_id", std::string());
            receipt.stateVersion = 0;
            ParseUnsigned(consumed, "state_version", receipt.stateVersion);
            ParseUnsigned(consumed, "at_epoch", receipt.consumedAtEpoch);
        }
    }
    if (receipt.id.rfind("ui-receipt-", 0) != 0 || receipt.projectId.empty() ||
        receipt.level.empty() || receipt.ttlSeconds == 0 || receipt.expiresAt.empty() ||
        (receipt.status != "active" && receipt.status != "consumed" &&
         receipt.status != "invalid")) {
        return false;
    }
    // 凭据只在短期进程生命周期内使用：重启后禁止继承自动执行权。
    if (receipt.status == "active") receipt.status = "invalid";
    out = std::move(receipt);
    return true;
}

} // namespace

bool ReceiptStore::ConfigurePersistence(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    receipts_.clear();
    nextId_ = 0;
    persistencePath_ = path;
    persistenceHealthy_ = false;
    if (path.empty()) {
        persistenceHealthy_ = true;
        return true;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            error = "unable to inspect receipt store: " + ec.message();
            return false;
        }
        persistenceHealthy_ = true;
        return true;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to read receipt store";
        return false;
    }
    Json document;
    try {
        input >> document;
    } catch (const std::exception&) {
        error = "invalid receipt store JSON";
        return false;
    }
    input.close();
    if (!document.is_object() ||
        document.value("schema_version", std::string()) != "edu.receipts.v1" ||
        !document.contains("receipts") || !document["receipts"].is_array()) {
        error = "unsupported receipt store format";
        return false;
    }
    std::uint64_t loadedNextId = 0;
    if (!ParseUnsigned(document, "next_id", loadedNextId)) {
        error = "invalid receipt store next_id";
        return false;
    }
    std::unordered_map<std::string, UiReceipt> loaded;
    std::uint64_t maximumSequence = 0;
    bool normalizedInvalid = false;
    for (const auto& item : document["receipts"]) {
        UiReceipt receipt;
        const std::string storedStatus = item.value("status", std::string());
        if (!ParseReceipt(item, receipt) || !loaded.emplace(receipt.id, receipt).second) {
            error = "invalid or duplicate receipt record";
            return false;
        }
        try {
            const std::string suffix = receipt.id.substr(std::string("ui-receipt-").size());
            std::size_t consumed = 0;
            const std::uint64_t sequence = std::stoull(suffix, &consumed);
            if (suffix.empty() || consumed != suffix.size() || sequence == 0) {
                error = "invalid receipt id sequence";
                return false;
            }
            maximumSequence = std::max(maximumSequence, sequence);
        } catch (const std::exception&) {
            error = "invalid receipt id sequence";
            return false;
        }
        if (storedStatus != receipt.status) normalizedInvalid = true;
    }
    if (loadedNextId < maximumSequence) {
        error = "receipt store next_id precedes an existing receipt";
        return false;
    }
    receipts_.swap(loaded);
    nextId_ = loadedNextId;
    persistenceHealthy_ = true;
    if (normalizedInvalid && !PersistLocked(error)) {
        persistenceHealthy_ = false;
        return false;
    }
    return true;
}

bool ReceiptStore::Issue(const std::string& projectId, const std::string& sessionId,
                         const std::string& issue, const std::string& level,
                         const std::string& policy, const std::string& revision,
                         const std::string& challengeId, const std::string& actionId,
                         std::uint64_t ttlSeconds, UiReceipt& out, std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t now = NowEpoch();
    UiReceipt receipt;
    receipt.id = "ui-receipt-" + std::to_string(++nextId_);
    receipt.projectId = projectId;
    receipt.sessionId = sessionId;
    receipt.issue = issue;
    receipt.level = level;
    receipt.policy = policy;
    receipt.revision = revision;
    receipt.challengeId = challengeId;
    receipt.actionId = actionId;
    receipt.status = "active";
    receipt.issuedAtEpoch = now;
    receipt.ttlSeconds = ttlSeconds == 0 ? 900 : ttlSeconds;
    receipt.expiresAt = FormatEpoch(now + receipt.ttlSeconds);
    receipts_[receipt.id] = receipt;
    if (!PersistLocked(error)) {
        receipts_.erase(receipt.id);
        --nextId_;
        return false;
    }
    out = receipt;
    return true;
}

bool ReceiptStore::Lookup(const std::string& receiptId, UiReceipt& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = receipts_.find(receiptId);
    if (it == receipts_.end()) return false;
    out = it->second;
    if (out.status == "active" && IsExpired(out, NowEpoch())) out.status = "invalid";
    return true;
}

bool ReceiptStore::Invalidate(const std::string& receiptId, std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = receipts_.find(receiptId);
    if (it == receipts_.end()) return false;
    const std::string previous = it->second.status;
    it->second.status = "invalid";
    if (!PersistLocked(error)) {
        it->second.status = previous;
        return false;
    }
    return true;
}

bool ReceiptStore::Consume(const std::string& receiptId, const std::string& projectId,
                           const std::string& runId, const std::string& actionId,
                           std::uint64_t expectedStateVersion, ReceiptDecision& decision,
                           UiReceipt& out, std::string& error) {
    error.clear();
    decision = ReceiptDecision::StoreUnavailable;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = receipts_.find(receiptId);
    if (it == receipts_.end()) {
        decision = ReceiptDecision::NotFound;
        error = "unknown receipt";
        return false;
    }
    UiReceipt& receipt = it->second;
    if (!projectId.empty() && receipt.projectId != projectId) {
        decision = ReceiptDecision::ProjectMismatch;
        error = "receipt belongs to another project";
        return false;
    }
    if (receipt.status == "invalid" || IsExpired(receipt, NowEpoch())) {
        decision = ReceiptDecision::Invalid;
        error = "receipt is no longer active";
        return false;
    }
    if (receipt.status == "consumed") {
        if (receipt.consumedActionId == actionId) {
            // 幂等重放：同一 action 重复核销返回原结果。
            decision = ReceiptDecision::Replayed;
            out = receipt;
            return true;
        }
        decision = ReceiptDecision::ActionMismatch;
        error = "receipt was consumed by another action";
        return false;
    }
    if (expectedStateVersion != receipt.stateVersion) {
        decision = ReceiptDecision::StateConflict;
        error = "state version does not match the issued receipt";
        return false;
    }
    receipt.status = "consumed";
    receipt.consumedByRunId = runId;
    receipt.consumedActionId = actionId;
    receipt.stateVersion = expectedStateVersion;
    receipt.consumedAtEpoch = NowEpoch();
    if (!PersistLocked(error)) {
        receipt.status = "active";
        receipt.consumedByRunId.clear();
        receipt.consumedActionId.clear();
        receipt.consumedAtEpoch = 0;
        return false;
    }
    decision = ReceiptDecision::Ok;
    out = receipt;
    return true;
}

bool ReceiptStore::PersistLocked(std::string& error) const {
    error.clear();
    if (!persistenceHealthy_) {
        error = "receipt store is unavailable";
        return false;
    }
    if (persistencePath_.empty()) return true;
    std::error_code ec;
    if (!persistencePath_.parent_path().empty()) {
        std::filesystem::create_directories(persistencePath_.parent_path(), ec);
        if (ec) {
            error = "unable to create receipt store directory: " + ec.message();
            return false;
        }
    }

    std::vector<const UiReceipt*> ordered;
    ordered.reserve(receipts_.size());
    for (const auto& entry : receipts_) ordered.push_back(&entry.second);
    std::sort(ordered.begin(), ordered.end(), [](const UiReceipt* left, const UiReceipt* right) {
        return left->id < right->id;
    });
    Json list = Json::array();
    for (const UiReceipt* receipt : ordered) list.push_back(ReceiptToJson(*receipt));
    const Json document{{"schema_version", "edu.receipts.v1"},
                        {"next_id", std::to_string(nextId_)},
                        {"receipts", list}};

    static std::atomic<std::uint64_t> temporarySequence{0};
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path temporary = std::filesystem::path(persistencePath_).concat(
        ".tmp." + std::to_string(ticks) + "." + std::to_string(temporarySequence.fetch_add(1)));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "unable to write receipt store temporary file";
            return false;
        }
        output << document.dump(2) << "\n";
        output.flush();
        if (!output) {
            error = "unable to flush receipt store temporary file";
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    std::filesystem::rename(temporary, persistencePath_, ec);
    if (ec) {
        std::error_code removeError;
        std::filesystem::remove(persistencePath_, removeError);
        ec.clear();
        std::filesystem::rename(temporary, persistencePath_, ec);
    }
    if (ec) {
        error = "unable to replace receipt store: " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

} // namespace agent
} // namespace eda
