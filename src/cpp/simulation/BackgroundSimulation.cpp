#include "BackgroundSimulation.h"
#include <limits>
#include <stdexcept>
#include <utility>

namespace hps::simulation
{
BackgroundSimulation::BackgroundSimulation(SimulationEngine engine,
    std::size_t batchSize, std::size_t queueCapacity)
    : controller_(std::move(engine)), batchSize_(batchSize), queueCapacity_(queueCapacity)
{
    if (batchSize == 0 || queueCapacity == 0)
        throw std::invalid_argument("Background batch size and queue capacity must be positive");
    Publish();
    // packaged_task delivers unexpected exceptions through an explicit future;
    // no catch-all handler converts them into success or a stale running state.
    std::packaged_task<void(std::stop_token)> task([this](std::stop_token token) {
        struct Finished
        {
            BackgroundSimulation& service;
            ~Finished()
            {
                {
                    std::lock_guard lock(service.mutex_);
                    service.closing_ = true;
                    // On unexpected failure, queued futures become broken promises
                    // immediately; callers can inspect Completion for the cause.
                    service.pending_.clear();
                }
                service.finished_.store(true, std::memory_order_release);
                service.wake_.notify_all();
            }
        } finished{*this};
        Worker(token);
    });
    completion_ = task.get_future().share();
    worker_ = std::jthread(std::move(task));
}

BackgroundSimulation::~BackgroundSimulation() { StopAndJoin(); }

void BackgroundSimulation::CheckCompletion() const
{
    if (finished_.load(std::memory_order_acquire)) completion_.get();
}

std::shared_ptr<const PublishedSimulation> BackgroundSimulation::Snapshot() const
{
    CheckCompletion();
    return published_.load(std::memory_order_acquire);
}

std::shared_ptr<const PublishedSimulation> BackgroundSimulation::WaitForUpdate(
    std::uint64_t after, std::chrono::milliseconds timeout) const
{
    {
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, timeout, [&] {
            return published_.load(std::memory_order_acquire)->revision > after ||
                finished_.load(std::memory_order_acquire);
        });
    }
    return Snapshot();
}

std::future<CommandReply> BackgroundSimulation::Submit(ServiceCommand command)
{
    CheckCompletion();
    // Reject invalid enum values before enqueueing work on another thread.
    switch (command)
    {
    case ServiceCommand::Resume: case ServiceCommand::Pause: case ServiceCommand::Stop:
    case ServiceCommand::ResetLegacy: case ServiceCommand::RestoreInitialSnapshot: break;
    default: throw std::invalid_argument("Unknown background simulation command");
    }
    Pending pending{command, {}};
    auto result = pending.reply.get_future();
    std::stop_source interrupt;
    {
        std::lock_guard lock(mutex_);
        if (closing_)
        {
            pending.reply.set_value({command, CommandOutcome::Closed, published_.load(std::memory_order_acquire)});
            return result;
        }
        if (pending_.size() == queueCapacity_) throw std::overflow_error("Background command queue is full");
        pending_.push_back(std::move(pending));
        if (command == ServiceCommand::Stop) interrupt = stopSource_;
        else if (command == ServiceCommand::Pause || command == ServiceCommand::ResetLegacy ||
            command == ServiceCommand::RestoreInitialSnapshot) interrupt = pauseSource_;
    }
    // Sources are per batch: a delayed signal cannot cancel a later resumed run.
    if (command != ServiceCommand::Resume) interrupt.request_stop();
    wake_.notify_all();
    return result;
}

void BackgroundSimulation::Publish(bool closed)
{
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Background publication revision overflow");
    auto next = std::make_shared<const PublishedSimulation>(PublishedSimulation{
        revision_ + 1, controller_.State(), controller_.Snapshot(), lastEnd_, issues_, closed});
    ++revision_;
    published_.store(std::move(next), std::memory_order_release);
    wake_.notify_all();
}

void BackgroundSimulation::Apply(Pending pending)
{
    auto outcome = CommandOutcome::Applied;
    switch (pending.command)
    {
    case ServiceCommand::Resume:
        switch (controller_.Resume())
        {
        case ResumeResult::Running: lastEnd_.reset(); break;
        case ResumeResult::ResetRequired: outcome = CommandOutcome::ResetRequired; break;
        case ResumeResult::SimulationStopped: outcome = CommandOutcome::SimulationStopped; break;
        }
        break;
    case ServiceCommand::Pause:
        controller_.Pause();
        if (controller_.State() == RunState::Paused) lastEnd_ = RunEndReason::Paused;
        break;
    case ServiceCommand::Stop:
        controller_.Stop();
        if (controller_.State() == RunState::Stopped) lastEnd_ = RunEndReason::Stopped;
        break;
    case ServiceCommand::ResetLegacy:
        controller_.ResetLegacy(); issues_.clear(); lastEnd_.reset(); break;
    case ServiceCommand::RestoreInitialSnapshot:
        controller_.RestoreInitialSnapshot(); issues_.clear(); lastEnd_.reset(); break;
    }
    Publish();
    pending.reply.set_value({pending.command, outcome, published_.load(std::memory_order_acquire)});
}

void BackgroundSimulation::Worker(std::stop_token shutdown)
{
    for (;;)
    {
        std::optional<Pending> command;
        RunRequests requests;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, shutdown, [&] {
                return closing_ || !pending_.empty() || controller_.State() == RunState::Running;
            });
            if (closing_ || shutdown.stop_requested()) break;
            if (!pending_.empty())
            {
                command.emplace(std::move(pending_.front())); pending_.pop_front();
            }
            else
            {
                pauseSource_ = std::stop_source{};
                stopSource_ = std::stop_source{};
                requests = {pauseSource_.get_token(), stopSource_.get_token()};
            }
        }
        if (command) Apply(std::move(*command));
        else
        {
            auto result = controller_.RunBatch(batchSize_, requests);
            lastEnd_ = result.reason;
            issues_ = std::move(result.issues);
            Publish();
        }
    }
    controller_.Stop();
    if (controller_.State() != RunState::Failed) lastEnd_ = RunEndReason::Stopped;
    Publish(true);
    std::deque<Pending> abandoned;
    {
        std::lock_guard lock(mutex_);
        abandoned.swap(pending_);
    }
    const auto final = published_.load(std::memory_order_acquire);
    for (auto& command : abandoned)
        command.reply.set_value({command.command, CommandOutcome::Closed, final});
}

void BackgroundSimulation::StopAndJoin()
{
    std::lock_guard join(joinMutex_);
    std::stop_source interrupt;
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
        interrupt = stopSource_;
    }
    interrupt.request_stop();
    worker_.request_stop();
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void BackgroundSimulation::Shutdown()
{
    StopAndJoin();
    completion_.get();
}
}
