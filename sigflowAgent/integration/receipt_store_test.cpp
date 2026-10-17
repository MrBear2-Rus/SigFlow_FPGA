// Caller supplies and owns the marked directory. This test never deletes files.
#include "eda-agent-gateway/ReceiptStore.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using eda::Json;
using eda::agent::ReceiptDecision;
using eda::agent::ReceiptStore;
using eda::agent::UiReceipt;

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto root = std::filesystem::u8path(argv[1]);
    if (!root.is_absolute() || !std::filesystem::is_regular_file(root / "test-owned")) return 2;
    int checks = 0, failures = 0;
    auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::cout << "FAIL " << name << '\n'; }
    };
    std::string error;
    ReceiptStore store;
    UiReceipt issued, result;
    ReceiptDecision decision;
    auto issue = [&](const std::string& action) {
        UiReceipt receipt;
        check(store.Issue("p1", "s1", "issue", "l4", "policy", "r1", "challenge",
                          action, 900, receipt, error), "issue receipt");
        return receipt;
    };
    auto consume = [&](const UiReceipt& receipt, const std::string& project,
                       const std::string& run, const std::string& action, std::uint64_t version = 0) {
        return store.Consume(receipt.id, project, run, action, version, decision, result, error);
    };
    issued = issue("confirm");
    check(!consume(issued, "p1", "run1", "other") && decision == ReceiptDecision::ActionMismatch,
          "issued action mismatch is rejected");
    check(store.Lookup(issued.id, result) && result.status == "active",
          "issued action mismatch leaves receipt active");
    issued = issue("confirm");
    check(consume(issued, "p1", "run1", "confirm"), "bound action consumes once");
    check(!consume(issued, "p1", "run2", "confirm"), "cross run replay is rejected");
    check(!consume(issued, "p1", "run1", "confirm", 1) && decision == ReceiptDecision::StateConflict,
          "replay state version mismatch is rejected");
    check(consume(issued, "p1", "run1", "confirm") && decision == ReceiptDecision::Replayed,
          "same run action version replays");
    for (int field = 0; field < 3; ++field) {
        auto scoped = issue("");
        check(!consume(scoped, field == 0 ? "" : "p1", field == 1 ? "" : "run1",
                       field == 2 ? "" : "confirm"), "empty project run or action is rejected");
        check(store.Lookup(scoped.id, result) && result.status == "active",
              "missing scope does not consume receipt");
    }
    auto legacy = issue("");
    check(consume(legacy, "p1", "run1", "legacy-action"), "legacy unbound issued action remains supported");
    check(!consume(legacy, "other", "run1", "legacy-action"), "cross project replay is rejected");

    // Hand-written, expired consumed record avoids a sleep or a production clock hook.
    const auto expiredPath = root / "expired-consumed.json";
    Json consumed{{"run_id", "run-old"}, {"action_id", "confirm-old"},
                  {"state_version", "7"}, {"at_epoch", "2"}};
    Json record{{"receipt_id", "ui-receipt-1"}, {"project_id", "p1"}, {"session_id", "s1"},
                {"issue", "issue"}, {"level", "l4"}, {"policy", "policy"}, {"revision", "r1"},
                {"challenge_id", "challenge-old"}, {"action_id", "confirm-old"}, {"status", "consumed"},
                {"expires_at", "1970-01-01T00:00:02Z"}, {"issued_at_epoch", "1"},
                {"ttl_seconds", "1"}, {"consumed", consumed}};
    { std::ofstream out(expiredPath); out << Json{{"schema_version", "edu.receipts.v1"},
                                                {"next_id", "1"}, {"receipts", Json::array({record})}}; }
    ReceiptStore expired;
    check(expired.ConfigurePersistence(expiredPath, error), "load expired consumed fixture");
    check(expired.Consume("ui-receipt-1", "p1", "run-old", "confirm-old", 7, decision, result, error)
              && decision == ReceiptDecision::Replayed && result.consumedAtEpoch == 2,
          "committed replay after TTL retains original result");
    check(!expired.Consume("ui-receipt-1", "p1", "run-other", "confirm-old", 7, decision, result, error),
          "expired consumed record rejects another run");
    check(expired.Invalidate("ui-receipt-1", error), "explicit invalidation succeeds");
    check(!expired.Consume("ui-receipt-1", "p1", "run-old", "confirm-old", 7, decision, result, error)
              && decision == ReceiptDecision::Invalid,
          "explicit invalidation denies committed replay");

    const auto persistedPath = root / "restart.json";
    ReceiptStore persisted;
    UiReceipt active, committed;
    check(persisted.ConfigurePersistence(persistedPath, error), "configure persistence");
    check(persisted.Issue("p1", "s1", "issue", "l4", "policy", "r1", "challenge", "",
                          900, active, error, 41) && active.stateVersion == 41,
          "issue active persistent receipt with explicit state version");
    check(persisted.Issue("p1", "s1", "issue", "l4", "policy", "r1", "challenge", "confirm",
                          900, committed, error, 42) && committed.stateVersion == 42,
          "issued state version is returned");
    check(!persisted.Consume(committed.id, "p1", "run1", "confirm", 0, decision, result, error)
              && decision == ReceiptDecision::StateConflict,
          "first consumption rejects wrong issued state version");
    check(persisted.Lookup(committed.id, result) && result.status == "active",
          "wrong issued version leaves receipt active");
    check(persisted.Consume(committed.id, "p1", "run1", "confirm", 42, decision, result, error),
          "persist first consumption");
    ReceiptStore reopened;
    check(reopened.ConfigurePersistence(persistedPath, error), "reopen store");
    check(reopened.Lookup(active.id, result) && result.status == "invalid" && result.stateVersion == 41,
          "restart invalidates active approval and preserves its issued version");
    check(reopened.Consume(committed.id, "p1", "run1", "confirm", 42, decision, result, error)
              && decision == ReceiptDecision::Replayed,
          "restart retains committed replay binding");
    for (const auto& malformed : {"7junk", "+7", "-1", " 7", "18446744073709551616"}) {
        record["consumed"]["state_version"] = malformed;
        { std::ofstream out(expiredPath); out << Json{{"schema_version","edu.receipts.v1"},
            {"next_id","1"},{"receipts",Json::array({record})}}; }
        ReceiptStore corrupt;
        check(!corrupt.ConfigurePersistence(expiredPath,error),"malformed persisted decimal is rejected");
    }
    std::cout << "receipt store: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
