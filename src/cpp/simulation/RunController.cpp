#include "RunController.h"
#include <utility>

namespace hps::simulation
{
RunController::RunController(SimulationEngine engine) : engine_(std::move(engine)) {}

ResumeResult RunController::Resume()
{
    if (state_ == RunState::Stopped || state_ == RunState::Failed) return ResumeResult::ResetRequired;
    if (engine_.Snapshot().stop.Locked()) return ResumeResult::SimulationStopped;
    state_ = RunState::Running;
    return ResumeResult::Running;
}

void RunController::Pause()
{
    if (state_ == RunState::Running) state_ = RunState::Paused;
}

void RunController::Stop()
{
    // Retain diagnostics/failure state until an explicit reset.
    if (state_ != RunState::Failed) state_ = RunState::Stopped;
}

RunResult RunController::RunBatch(std::size_t stepBudget, const RunRequests& requests)
{
    std::size_t completed = 0;
    auto finish = [&](RunEndReason reason) {
        return RunResult{reason, completed, engine_.Snapshot(), lastIssues_};
    };
    for (;;)
    {
        if (state_ == RunState::Failed) return finish(RunEndReason::StepError);
        if (state_ == RunState::Stopped) return finish(RunEndReason::Stopped);
        if (requests.stop.stop_requested())
        {
            Stop();
            return finish(RunEndReason::Stopped);
        }
        if (requests.pause.stop_requested())
        {
            Pause();
            return finish(RunEndReason::Paused);
        }
        if (state_ == RunState::Paused) return finish(RunEndReason::Paused);
        if (completed == stepBudget) return finish(RunEndReason::BudgetExhausted);
        auto result = engine_.StepSeeded();
        if (auto* issues = std::get_if<std::vector<domain::ValidationIssue>>(&result))
        {
            lastIssues_ = std::move(*issues);
            state_ = RunState::Failed;
            return finish(RunEndReason::StepError);
        }
        ++completed;
        if (std::get<SeededStepPlan>(result).step.stop.Locked())
        {
            state_ = RunState::Paused;
            return finish(RunEndReason::SimulationStopped);
        }
    }
}

void RunController::ResetLegacy()
{
    engine_.ResetLegacy();
    lastIssues_.clear();
    state_ = RunState::Paused;
}

void RunController::RestoreInitialSnapshot()
{
    engine_.RestoreInitialSnapshot();
    lastIssues_.clear();
    state_ = RunState::Paused;
}
}
