#include "eda-agent-gateway/GrantStore.h"
#include "eda-agent-gateway/SnapshotService.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool ok, const char* message) {
    if (ok) {
        std::cout << "  ok: " << message << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL: " << message << "\n";
    }
}

void WriteText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string ReadText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void SetSnapshotEpoch(const fs::path& manifestPath, std::uint64_t epoch) {
    eda::Json manifest = eda::Json::parse(ReadText(manifestPath));
    manifest["created_at_epoch"] = std::to_string(epoch);
    WriteText(manifestPath, manifest.dump(2) + "\n");
}

fs::path TestRoot() {
    const auto value = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("sigflow_edu_storage_" + std::to_string(value));
}

} // namespace

int main() {
    const fs::path root = TestRoot();
    const fs::path projectRoot = root / "project with space";
    const fs::path storageRoot = projectRoot / ".sigflow" / "agent" / "snapshots";
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
    WriteText(projectRoot / "rtl" / "top.v", "module top; wire a; endmodule\n");
    WriteText(projectRoot / "rtl" / "包含 空格.v", "module child; endmodule\n");

    eda::agent::SnapshotRequest request;
    request.projectId = "project-1";
    request.projectRoot = projectRoot;
    request.expectedRevision = "rev-1";
    request.currentRevision = "rev-1";
    request.top = "top";
    request.sources = {{"source-child", fs::path("rtl") / "包含 空格.v"},
                       {"source-top", fs::path("rtl") / "top.v"}};
    request.target = eda::Json{{"profile", "tang-nano-9k"}};
    request.toolConfig = eda::Json{{"provider", "synth"}, {"opt", "area"}};

    eda::agent::SnapshotService snapshots(storageRoot);
    const eda::agent::SnapshotResult first = snapshots.Create(request);
    Check(static_cast<bool>(first), "snapshot creates from saved synchronized inputs");
    Check(first.snapshot.id.rfind("snap-", 0) == 0, "snapshot has opaque id");
    Check(first.snapshot.inputFingerprint.size() == 64, "snapshot has SHA-256 input fingerprint");
    Check(first.snapshot.sources.size() == 2, "snapshot records registered sources only");
    Check(first.snapshot.sources[0].relativePath == "rtl/top.v",
          "snapshot source manifest has deterministic path order");
    const fs::path frozenTop = snapshots.SnapshotDirectory(first.snapshot.id) / "files" / "rtl" / "top.v";
    Check(ReadText(frozenTop) == "module top; wire a; endmodule\n",
          "snapshot contains an immutable source copy");

    eda::agent::SnapshotRecord loaded;
    std::string error;
    eda::agent::SnapshotService restarted(storageRoot);
    Check(restarted.Lookup(first.snapshot.id, loaded, error), "snapshot manifest reloads after restart");
    Check(loaded.inputFingerprint == first.snapshot.inputFingerprint,
          "reloaded snapshot preserves fingerprint");
    Check(ReadText(snapshots.SnapshotDirectory(first.snapshot.id) / "manifest.json")
              .find(projectRoot.string()) == std::string::npos,
          "snapshot manifest does not expose the project absolute path");

    eda::agent::SnapshotRequest reordered = request;
    std::reverse(reordered.sources.begin(), reordered.sources.end());
    const eda::agent::SnapshotResult sameInputs = snapshots.Create(reordered);
    Check(static_cast<bool>(sameInputs), "same inputs may create another immutable snapshot");
    Check(sameInputs.snapshot.inputFingerprint == first.snapshot.inputFingerprint,
          "input fingerprint is independent of source enumeration order");
    // 相同输入生成相同 id；"不同 snapshot_id" 由不同输入保证（见下方 changedResult）。
    Check(!sameInputs.snapshot.id.empty() && sameInputs.snapshot.id == first.snapshot.id,
          "identical inputs map to the same immutable snapshot id");
    Check(fs::exists(snapshots.SnapshotDirectory(sameInputs.snapshot.id) / "manifest.json"),
          "identical-input snapshot is readable");

    // 并发/重复提交回归：同输入的快照目录已存在时，Create 必须成功返回既有快照，
    // 而不是在目录提交处失败（此前的偶发 503 unable to commit snapshot directory）。
    // 注意：必须在本用例改动 top.v 之前执行，否则输入已变、指纹不同。
    {
        eda::agent::SnapshotRequest repeat = request;
        const eda::agent::SnapshotResult again = snapshots.Create(repeat);
        Check(static_cast<bool>(again), "repeat commit of an existing snapshot directory succeeds");
        Check(again.snapshot.id == first.snapshot.id,
              "repeat commit resolves to the existing snapshot id");
        Check(fs::exists(snapshots.SnapshotDirectory(again.snapshot.id) / "manifest.json"),
              "existing snapshot manifest survives a repeat commit");

        bool allSucceeded = true;
        for (int i = 0; i < 5; ++i) {
            const eda::agent::SnapshotResult loop = snapshots.Create(repeat);
            if (!loop || loop.snapshot.id != first.snapshot.id) allSucceeded = false;
        }
        Check(allSucceeded, "repeated identical commits are idempotent");
    }

    WriteText(projectRoot / "rtl" / "top.v", "module top; wire b; endmodule\n");
    Check(ReadText(frozenTop) == "module top; wire a; endmodule\n",
          "editing the project does not mutate an existing snapshot");
    eda::agent::SnapshotRequest changed = request;
    changed.expectedRevision = "rev-2";
    changed.currentRevision = "rev-2";
    const eda::agent::SnapshotResult changedResult = snapshots.Create(changed);
    Check(static_cast<bool>(changedResult), "new revision creates a new snapshot");
    Check(changedResult.snapshot.inputFingerprint != first.snapshot.inputFingerprint,
          "source changes alter the input fingerprint");

    eda::agent::SnapshotRequest externallyChanged = changed;
    for (auto& source : externallyChanged.sources) {
        if (source.sourceId == "source-top") source.expectedSha256 = std::string(64, '0');
    }
    Check(snapshots.Create(externallyChanged).code ==
              eda::agent::SnapshotError::SourceChanged,
          "source content differing from the observed revision hash is rejected");
    WriteText(projectRoot / "sigflow.project", "{}\n");
    eda::agent::SnapshotRequest changedManifest = changed;
    changedManifest.expectedProjectManifestSha256 = std::string(64, '0');
    Check(snapshots.Create(changedManifest).code ==
              eda::agent::SnapshotError::SourceChanged,
          "project manifest differing from the observed revision hash is rejected");

    eda::agent::SnapshotRequest dirty = changed;
    dirty.dirty = true;
    Check(snapshots.Create(dirty).code == eda::agent::SnapshotError::DirtyProject,
          "dirty editor state is rejected");
    eda::agent::SnapshotRequest syncing = changed;
    syncing.synchronized = false;
    Check(snapshots.Create(syncing).code == eda::agent::SnapshotError::SyncInProgress,
          "incomplete graph/code synchronization is rejected");
    eda::agent::SnapshotRequest stale = changed;
    stale.expectedRevision = "rev-1";
    Check(snapshots.Create(stale).code == eda::agent::SnapshotError::StaleRevision,
          "stale expected revision is rejected");
    eda::agent::SnapshotRequest traversal = changed;
    traversal.sources = {{"escape", fs::path("..") / "outside.v"}};
    Check(snapshots.Create(traversal).code == eda::agent::SnapshotError::PathOutsideProject,
          "parent traversal source path is rejected");
    eda::agent::SnapshotRequest absolute = changed;
    absolute.sources = {{"absolute", projectRoot / "rtl" / "top.v"}};
    Check(snapshots.Create(absolute).code == eda::agent::SnapshotError::PathOutsideProject,
          "absolute source path is rejected");
    const fs::path outside = root / "outside.v";
    const fs::path symlink = projectRoot / "rtl" / "escape-link.v";
    WriteText(outside, "module outside; endmodule\n");
    std::error_code symlinkError;
    fs::create_symlink(outside, symlink, symlinkError);
    if (!symlinkError) {
        eda::agent::SnapshotRequest linked = changed;
        linked.sources = {{"linked", fs::path("rtl") / "escape-link.v"}};
        Check(snapshots.Create(linked).code == eda::agent::SnapshotError::PathOutsideProject,
              "symlink escaping the project root is rejected");
    }

    int probeCalls = 0;
    const eda::agent::SnapshotResult changedDuringBuild = snapshots.Create(
        changed, [&probeCalls]() {
            ++probeCalls;
            return probeCalls == 1 ? std::string("rev-2") : std::string("rev-3");
        });
    Check(changedDuringBuild.code == eda::agent::SnapshotError::StaleRevision,
          "revision change during snapshot creation is rejected");

    eda::agent::SnapshotService::Limits tinyLimits;
    tinyLimits.maxSources = 2;
    tinyLimits.maxTotalBytes = 4;
    eda::agent::SnapshotService tiny(storageRoot / "tiny", tinyLimits);
    Check(tiny.Create(changed).code == eda::agent::SnapshotError::InvalidArgument,
          "snapshot byte limit is enforced");

    // 另建一个未被引用的旧快照，用于验证保留策略的删除分支。
    eda::agent::SnapshotRequest obsolete = changed;
    obsolete.expectedRevision = "rev-obsolete";
    obsolete.currentRevision = "rev-obsolete";
    const eda::agent::SnapshotResult obsoleteResult = snapshots.Create(obsolete);
    Check(static_cast<bool>(obsoleteResult) &&
              obsoleteResult.snapshot.id != first.snapshot.id,
          "obsolete revision snapshot created for retention");

    SetSnapshotEpoch(snapshots.SnapshotDirectory(first.snapshot.id) / "manifest.json", 1);
    SetSnapshotEpoch(snapshots.SnapshotDirectory(obsoleteResult.snapshot.id) / "manifest.json", 2);
    SetSnapshotEpoch(snapshots.SnapshotDirectory(changedResult.snapshot.id) / "manifest.json", 3);
    std::size_t removedSnapshots = 0;
    error.clear();
    Check(snapshots.Prune({first.snapshot.id}, 1, 10, removedSnapshots, error, 100),
          "snapshot retention prune succeeds");
    Check(removedSnapshots == 1, "retention removes only old unprotected snapshots");
    Check(fs::exists(snapshots.SnapshotDirectory(first.snapshot.id)),
          "protected snapshot survives retention");
    Check(!fs::exists(snapshots.SnapshotDirectory(obsoleteResult.snapshot.id)),
          "old unprotected snapshot is removed");
    Check(fs::exists(snapshots.SnapshotDirectory(changedResult.snapshot.id)),
          "most recent snapshot survives retention");

    const fs::path externalRoot = root / "approved includes";
    const fs::path externalFile = externalRoot / "defs.vh";
    WriteText(externalFile, "`define WIDTH 8\n");
    // 独立 revision：避免快照 id 与上面的 changed 相同而在 commit 时走"已存在"分支，
    // 那样 externalResult 指向的目录不包含本用例的外部 include。
    eda::agent::SnapshotRequest external = changed;
    external.expectedRevision = "rev-3";
    external.currentRevision = "rev-3";
    external.sources.push_back({"external-defs", {}, externalFile});
    external.allowedExternalRoots = {externalRoot};
    const eda::agent::SnapshotResult externalResult = snapshots.Create(external);
    Check(static_cast<bool>(externalResult), "allowlisted external include is snapshotted");
    const fs::path copiedExternal = snapshots.SnapshotDirectory(externalResult.snapshot.id) /
                                    "files" / "external" / "external-defs" / "defs.vh";
    Check(ReadText(copiedExternal) == "`define WIDTH 8\n",
          "external include is copied under an opaque snapshot path");
    Check(ReadText(snapshots.SnapshotDirectory(externalResult.snapshot.id) / "manifest.json")
              .find(externalRoot.string()) == std::string::npos,
          "external include absolute path is absent from the manifest");
    eda::agent::SnapshotRequest deniedExternal = external;
    deniedExternal.allowedExternalRoots.clear();
    Check(snapshots.Create(deniedExternal).code ==
              eda::agent::SnapshotError::PathOutsideProject,
          "external include outside the allowlist is rejected");

    const fs::path grantPath = projectRoot / ".sigflow" / "agent" / "grants.json";
    eda::agent::GrantStore grants;
    Check(grants.ConfigurePersistence(grantPath, error), "grant persistence opens an empty store");
    eda::agent::Grant grant1;
    error.clear();
    Check(grants.Issue("project-1", "plan-hash-1", "rev-2", changedResult.snapshot.id,
                       eda::Json::array({eda::Json{{"step_id", "synth-1"}}}), 600, 3,
                       grant1, error),
          "grant issue persists successfully");
    Check(fs::is_regular_file(grantPath), "grant index is written to disk");
    Check(grants.Check(grant1.id, "other-project") ==
              eda::agent::GrantDecision::ProjectMismatch,
          "live grant enforces project isolation check");

    eda::agent::GrantStore reloadedGrants;
    error.clear();
    Check(reloadedGrants.ConfigurePersistence(grantPath, error), "grant index reloads after restart");
    eda::agent::Grant loadedGrant;
    Check(reloadedGrants.Lookup(grant1.id, loadedGrant), "persisted grant is found after restart");
    Check(loadedGrant.snapshotId == changedResult.snapshot.id &&
              loadedGrant.steps.size() == 1 && loadedGrant.status == "expired",
          "restart preserves grant evidence but invalidates execution authority");
    Check(reloadedGrants.Check(grant1.id, "project-1") ==
              eda::agent::GrantDecision::Expired,
          "restarted process cannot execute with an old grant");

    eda::agent::Grant grant2;
    error.clear();
    Check(reloadedGrants.Issue("project-1", "plan-hash-2", "rev-2",
                               changedResult.snapshot.id, eda::Json::array(), 600, 1,
                               grant2, error),
          "second grant persists");
    Check(grant2.id != grant1.id, "grant id sequence survives restart");
    error.clear();
    Check(reloadedGrants.Revoke(grant1.id, error), "grant revocation persists");
    eda::agent::GrantStore revokedReload;
    Check(revokedReload.ConfigurePersistence(grantPath, error), "revoked grant store reloads");
    Check(revokedReload.Lookup(grant1.id, loadedGrant) && loadedGrant.status == "revoked",
          "revoked state survives restart");

    eda::agent::Grant expiring;
    error.clear();
    Check(revokedReload.Issue("project-1", "plan-expire", "rev-2",
                              changedResult.snapshot.id, eda::Json::array(), 1, 1,
                              expiring, error),
          "short-lived grant persists");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    Check(revokedReload.Lookup(expiring.id, loadedGrant) && loadedGrant.status == "expired",
          "grant lookup reports effective expiry");
    Check(revokedReload.Check(expiring.id, "project-1") == eda::agent::GrantDecision::Expired,
          "expired grant is rejected by execution check");

    const fs::path blocker = root / "not-a-directory";
    WriteText(blocker, "block");
    eda::agent::GrantStore failingStore;
    error.clear();
    Check(failingStore.ConfigurePersistence(blocker / "grants.json", error),
          "grant store may be configured before its first write");
    eda::agent::Grant failedGrant;
    error.clear();
    Check(!failingStore.Issue("project-1", "plan-fail", "rev-2",
                              changedResult.snapshot.id, eda::Json::array(), 600, 1,
                              failedGrant, error),
          "grant issue fails when persistence cannot commit");
    Check(!error.empty(), "grant persistence failure returns an error");
    Check(!failingStore.Lookup("grant-1", loadedGrant),
          "failed grant persistence rolls back the in-memory grant");

    const fs::path corruptPath = root / "corrupt-grants.json";
    WriteText(corruptPath, "{not-json");
    eda::agent::GrantStore corruptStore;
    error.clear();
    Check(!corruptStore.ConfigurePersistence(corruptPath, error),
          "corrupt grant store fails closed");

    // next_id 不能落后于已有记录，否则重启后的新签发会覆盖旧 grant。
    eda::Json inconsistent = eda::Json::parse(ReadText(grantPath));
    inconsistent["next_id"] = "0";
    const fs::path inconsistentPath = root / "inconsistent-grants.json";
    WriteText(inconsistentPath, inconsistent.dump());
    eda::agent::GrantStore inconsistentStore;
    error.clear();
    Check(!inconsistentStore.ConfigurePersistence(inconsistentPath, error),
          "grant store rejects an id sequence that could collide");

    WriteText(frozenTop, "tampered\n");
    error.clear();
    Check(!restarted.Lookup(first.snapshot.id, loaded, error),
          "snapshot reload detects copied source tampering");

    // SF-03：Grant 配额消耗（max_jobs）——原子递增、超限拒绝、撤销后拒绝。
    {
        eda::agent::GrantStore quota;
        error.clear();
        eda::agent::Grant issued;
        Check(quota.Issue("project-quota", "plan-q", "rev-1", "snap-1", eda::Json::array(),
                          600, 2, issued, error),
              "quota: grant issued with max_jobs=2");
        Check(issued.usedJobs == 0, "quota: used_jobs starts at 0");

        eda::agent::GrantDecision decision = eda::agent::GrantDecision::NotFound;
        Check(quota.Consume(issued.id, "project-quota", decision, error) &&
                  decision == eda::agent::GrantDecision::Ok,
              "quota: first consume ok");
        Check(quota.Consume(issued.id, "project-quota", decision, error) &&
                  decision == eda::agent::GrantDecision::Ok,
              "quota: second consume ok");

        error.clear();
        Check(!quota.Consume(issued.id, "project-quota", decision, error) &&
                  decision == eda::agent::GrantDecision::ProjectExhausted,
              "quota: third consume rejected (exhausted)");

        eda::agent::Grant reloaded;
        Check(quota.Lookup(issued.id, reloaded) && reloaded.usedJobs == 2,
              "quota: used_jobs persisted at max");

        // 跨项目消耗被拒。
        error.clear();
        Check(!quota.Consume(issued.id, "project-other", decision, error) &&
                  decision == eda::agent::GrantDecision::ProjectMismatch,
              "quota: cross-project consume rejected");

        // 撤销后不可消耗。
        Check(quota.Revoke(issued.id, error), "quota: grant revoked");
        error.clear();
        Check(!quota.Consume(issued.id, "project-quota", decision, error) &&
                  decision == eda::agent::GrantDecision::Revoked,
              "quota: revoked grant cannot consume");
    }

    fs::remove_all(root, cleanupError);
    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
