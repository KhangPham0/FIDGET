#ifndef FIDGET_HARDWARE_TUNING_SESSION_COORDINATOR_H
#define FIDGET_HARDWARE_TUNING_SESSION_COORDINATOR_H

#include "core/ApplicationStorage.h"
#include "core/TunerControl.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace fidget {

class CommandWorker;
class ITransportFactory;

using TuningSessionSnapshotReader =
    std::function<std::shared_ptr<const TunerSnapshot>()>;
using TuningSessionSnapshotPublisher =
    std::function<void(TunerSnapshot)>;

// Coordinates the project-independent GUI target path. Hardware work stays on
// the shared command worker and depends only on ITransportFactory endpoint
// requests. The legacy project-guided workflow remains outside this class.
class TuningSessionCoordinator
{
public:
    TuningSessionCoordinator(
        ITransportFactory& transportFactory,
        CommandWorker& worker,
        ApplicationStoragePaths storagePaths,
        TuningSessionSnapshotReader snapshotReader,
        TuningSessionSnapshotPublisher snapshotPublisher);

    TuningSessionCoordinator(const TuningSessionCoordinator&) = delete;
    TuningSessionCoordinator& operator=(
        const TuningSessionCoordinator&) = delete;

    [[nodiscard]] static bool Handles(
        const TunerCommand& command) noexcept;
    void Submit(TunerCommand command);
    void CancelPendingProbe() noexcept;

private:
    void EditTarget(EditTunerTargetCommand command);
    void ConnectController(
        const std::shared_ptr<std::atomic<bool>>& cancellation);
    void ProbeTarget(
        const std::shared_ptr<std::atomic<bool>>& cancellation);
    void OpenTargetSession();
    void ClearTarget();
    void SetWorkspace(SetTunerWorkspaceCommand command);
    void ClearWorkspace();

    static void RefreshWorkspaceTargetEvidence(TunerSnapshot& snapshot);

    [[nodiscard]] std::shared_ptr<std::atomic<bool>> BeginProbe();
    void FinishProbe(
        const std::shared_ptr<std::atomic<bool>>& cancellation) noexcept;
    [[nodiscard]] TunerSnapshot SnapshotCopy() const;
    void PublishStatus(
        TunerSnapshot snapshot,
        TunerStatusLevel level,
        std::string summary,
        std::string detail = {});
    static void RefreshPresentationEvidence(TunerSnapshot& snapshot);

    ITransportFactory& transportFactory_;
    CommandWorker& worker_;
    ApplicationStoragePaths storagePaths_;
    TuningSessionSnapshotReader snapshotReader_;
    TuningSessionSnapshotPublisher snapshotPublisher_;
    std::mutex probeMutex_;
    std::shared_ptr<std::atomic<bool>> activeProbeCancellation_;
};

} // namespace fidget

#endif
