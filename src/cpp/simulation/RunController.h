#pragma once
#include "SimulationEngine.h"
#include <stop_token>

namespace hps::simulation
{
enum class RunState { Paused, Running, Stopped, Failed };
enum class ResumeResult { Running, ResetRequired, SimulationStopped };
enum class RunEndReason { BudgetExhausted, Paused, Stopped, SimulationStopped, StepError };
struct RunRequests
{
    std::stop_token pause;
    std::stop_token stop;
};
struct RunResult
{
    RunEndReason reason;
    std::size_t completedSteps;
    EngineSnapshot snapshot;
    std::vector<domain::ValidationIssue> issues;
};

// Synchronous owner. Only request tokens may be signalled concurrently; all
// controller methods/snapshot reads require serialized access. No worker exists.
class RunController
{
public:
    explicit RunController(SimulationEngine engine);
    RunState State() const { return state_; }
    EngineSnapshot Snapshot() const { return engine_.Snapshot(); }
    ResumeResult Resume();
    void Pause();
    void Stop();
    // Budget counts successful complete steps in this call, not total lifetime.
    // Requests are checked between steps. A failed step retains the last success.
    RunResult RunBatch(std::size_t stepBudget, const RunRequests& requests = {});
    void ResetLegacy();
    void RestoreInitialSnapshot();

private:
    SimulationEngine engine_;
    RunState state_ = RunState::Paused;
    std::vector<domain::ValidationIssue> lastIssues_;
};
}
