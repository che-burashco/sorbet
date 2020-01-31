#include "core/lsp/TypecheckEpochManager.h"
#include "core/lsp/PreemptionTaskManager.h"

using namespace std;
namespace sorbet::core::lsp {

void TypecheckEpochManager::assertConsistentThread(optional<thread::id> &expectedThreadId, string_view method,
                                                   string_view threadName) {
    if (!expectedThreadId.has_value()) {
        expectedThreadId = this_thread::get_id();
    } else if (*expectedThreadId != this_thread::get_id()) {
        Exception::raise("{} can only be called by the {} thread.", method, threadName);
    }
}

void TypecheckEpochManager::startCommitEpoch(u4 fromEpoch, u4 toEpoch) {
    absl::MutexLock lock(&epochMutex);
    ENFORCE(fromEpoch != toEpoch);
    ENFORCE(toEpoch != currentlyProcessingLSPEpoch.load());
    ENFORCE(toEpoch != lastCommittedLSPEpoch.load());
    // epoch should be a version 'ahead' of currentlyProcessingLSPEpoch. The distance between the two is the number of
    // fast path edits that have come in since the last slow path. Since epochs overflow, there's nothing that I can
    // easily assert here to ensure that we are not moving backward in time.
    currentlyProcessingLSPEpoch.store(toEpoch);
    lspEpochInvalidator.store(toEpoch);
    // lastCommittedLSPEpoch currently contains the epoch of the last slow path we processed. Since then, we may have
    // committed several fast paths. So, update it to the epoch of the last fast path committed.
    // We do it this way rather than keep it up-to-date after every fast path to reduce footguns, especially in testing.
    // With this design, when starting a commit epoch, you have to specify the (from, to] range, and it is compiler
    // enforced.
    lastCommittedLSPEpoch.store(fromEpoch);
}

bool TypecheckEpochManager::wasTypecheckingCanceled() const {
    // This method is called from many worker threads. Locking isn't required; the result can be slightly out-of-date.
    return ABSL_TS_UNCHECKED_READ(lspEpochInvalidator).load() !=
           ABSL_TS_UNCHECKED_READ(currentlyProcessingLSPEpoch).load();
}

TypecheckEpochManager::TypecheckingStatus TypecheckEpochManager::getStatusInternal() const {
    const u4 processing = currentlyProcessingLSPEpoch.load();
    const u4 committed = lastCommittedLSPEpoch.load();
    const u4 invalidator = lspEpochInvalidator.load();
    const bool slowPathRunning = processing != committed;
    const bool slowPathIsCanceled = processing != invalidator;
    return TypecheckingStatus{slowPathRunning, slowPathIsCanceled, committed, processing};
}

TypecheckEpochManager::TypecheckingStatus TypecheckEpochManager::getStatus() const {
    absl::MutexLock lock(&epochMutex);
    return getStatusInternal();
}

bool TypecheckEpochManager::tryCancelSlowPath(u4 newEpoch) {
    assertConsistentThread(preprocessThreadId, "TypecheckEpochManager::tryCancelSlowPath", "preprocessThread");
    absl::MutexLock lock(&epochMutex);
    const u4 processing = currentlyProcessingLSPEpoch.load();
    ENFORCE(newEpoch != processing); // This would prevent a cancelation from happening.
    const u4 committed = lastCommittedLSPEpoch.load();
    // The second condition should never happen, but guard against it in production.
    if (processing == committed || newEpoch == processing) {
        return false;
    }
    // Cancel slow path by bumping invalidator.
    lspEpochInvalidator.store(newEpoch);
    return true;
}

bool TypecheckEpochManager::tryCommitEpoch(u4 epoch, bool isCancelable,
                                           optional<shared_ptr<PreemptionTaskManager>> preemptionManager,
                                           function<void()> typecheck) {
    assertConsistentThread(typecheckingThreadId, "TypecheckEpochManager::tryCommitEpoch", "typechecking");
    if (!isCancelable) {
        typecheck();
        return true;
    }

    // Should have called "startCommitEpoch" *before* this method.
    ENFORCE(ABSL_TS_UNCHECKED_READ(currentlyProcessingLSPEpoch).load() == epoch);
    // Typechecking does not run under the mutex, as it would prevent another thread from running `tryCancelSlowPath`
    // during typechecking.
    typecheck();

    bool committed = false;
    {
        absl::MutexLock lock(&epochMutex);
        // Try to commit.
        const u4 processing = currentlyProcessingLSPEpoch.load();
        const u4 invalidator = lspEpochInvalidator.load();
        if (processing == invalidator) {
            ENFORCE(lastCommittedLSPEpoch.load() != processing, "Trying to commit an already-committed epoch.");
            // OK to commit!
            lastCommittedLSPEpoch.store(processing);
            committed = true;
        } else {
            // Typechecking was canceled.
            const u4 lastCommitted = lastCommittedLSPEpoch.load();
            currentlyProcessingLSPEpoch.store(lastCommitted);
            lspEpochInvalidator.store(lastCommitted);
        }
    }

    if (preemptionManager.has_value()) {
        // Now that we are no longer running a slow path, run a preemption task that might have snuck in while we were
        // finishing up. No others can be scheduled.
        (*preemptionManager)->tryRunScheduledPreemptionTask();
    }
    return committed;
}

void TypecheckEpochManager::withEpochLock(function<void(TypecheckingStatus)> lambda) const {
    absl::MutexLock lock(&epochMutex);
    lambda(getStatusInternal());
}

} // namespace sorbet::core::lsp