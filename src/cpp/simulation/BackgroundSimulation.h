#pragma once
#include "RunController.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace hps::simulation
{
enum class ServiceCommand { Resume, Pause, Stop, ResetLegacy, RestoreInitialSnapshot };
enum class CommandOutcome { Applied, ResetRequired, SimulationStopped, Closed };
struct PublishedSimulation
{
    std::uint64_t revision;
    RunState state;
    EngineSnapshot engine;
    std::optional<RunEndReason> lastEnd;
    std::vector<domain::ValidationIssue> issues;
    bool closed;
};
struct CommandReply
{
    ServiceCommand command;
    CommandOutcome outcome;
    std::shared_ptr<const PublishedSimulation> snapshot;
};

class BackgroundSimulation
{
public:
    explicit BackgroundSimulation(SimulationEngine engine,
        std::size_t batchSize = 16, std::size_t queueCapacity = 64);
    ~BackgroundSimulation();
    BackgroundSimulation(const BackgroundSimulation&) = delete;
    BackgroundSimulation& operator=(const BackgroundSimulation&) = delete;
    BackgroundSimulation(BackgroundSimulation&&) = delete;
    BackgroundSimulation& operator=(BackgroundSimulation&&) = delete;

    std::future<CommandReply> Submit(ServiceCommand command);
    std::shared_ptr<const PublishedSimulation> Snapshot() const;
    // Timeout may return the same revision. Completion/failure also wakes waiters.
    std::shared_ptr<const PublishedSimulation> WaitForUpdate(std::uint64_t after,
        std::chrono::milliseconds timeout) const;
    std::shared_future<void> Completion() const { return completion_; }
    // Serialized join, without holding the command mutex. Rethrows worker errors.
    void Shutdown();

private:
    struct Pending
    {
        ServiceCommand command;
        std::promise<CommandReply> reply;
    };
    void Worker(std::stop_token shutdown);
    void Publish(bool closed = false);
    void Apply(Pending pending);
    void StopAndJoin();
    void CheckCompletion() const;

    RunController controller_; // Accessed by worker only after construction.
    std::size_t batchSize_;
    std::size_t queueCapacity_;
    std::uint64_t revision_ = 0;
    std::optional<RunEndReason> lastEnd_;
    std::vector<domain::ValidationIssue> issues_;
    mutable std::mutex mutex_;
    mutable std::condition_variable_any wake_;
    std::mutex joinMutex_;
    std::deque<Pending> pending_;
    bool closing_ = false;
    std::stop_source pauseSource_, stopSource_;
    std::atomic<std::shared_ptr<const PublishedSimulation>> published_;
    std::atomic<bool> finished_ = false;
    std::shared_future<void> completion_;
    std::jthread worker_; // Joined before any state above is destroyed.
};
}
