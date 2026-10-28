#include "DocumentSimulationSession.h"
#include <limits>
#include <stdexcept>
#include <utility>

namespace hps::application
{
namespace
{
SessionId NextSessionId()
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;)
    {
        if (value == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Simulation session ID space exhausted");
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return {value};
    }
}
}

DocumentSimulationSession::DocumentSimulationSession(SessionIdentity identity,
    simulation::SimulationEngine engine, const SessionOptions& options)
    : identity_(identity), owner_(std::this_thread::get_id()),
      service_(std::move(engine), options.batchSize, options.queueCapacity)
{
}

SessionCreationResult DocumentSimulationSession::Create(DocumentId document,
    const domain::NetSnapshot& snapshot, const domain::ConflictTopology& topology,
    const SessionOptions& options)
{
    if (document.value == 0)
        return std::vector<domain::ValidationIssue>{{domain::ValidationCode::InvalidId, 0, "Document ID must be nonzero"}};
    if (options.batchSize == 0 || options.queueCapacity == 0)
        return std::vector<domain::ValidationIssue>{{domain::ValidationCode::InvalidSimulationSettings, 0,
            "Session batch size and queue capacity must be positive"}};
    auto created = simulation::SimulationEngine::Create(snapshot, topology, options.clock, options.seed);
    if (auto* issues = std::get_if<std::vector<domain::ValidationIssue>>(&created)) return std::move(*issues);
    return std::unique_ptr<DocumentSimulationSession>(new DocumentSimulationSession(
        {document, NextSessionId()}, std::get<simulation::SimulationEngine>(std::move(created)), options));
}

void DocumentSimulationSession::RequireOwner() const
{
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("Document simulation session must be accessed on its owning thread");
}

SessionIdentity DocumentSimulationSession::Identity() const { RequireOwner(); return identity_; }
bool DocumentSimulationSession::IsClosed() const { RequireOwner(); return closed_; }
bool DocumentSimulationSession::Accepts(SessionIdentity identity) const
{
    RequireOwner();
    return !closed_ && identity == identity_;
}

SessionCommand DocumentSimulationSession::Submit(simulation::ServiceCommand command)
{
    RequireOwner();
    if (closed_) throw std::logic_error("Document simulation session is closed");
    return {identity_, service_.Submit(command)};
}

SessionSnapshot DocumentSimulationSession::Snapshot() const
{
    RequireOwner();
    return {identity_, service_.Snapshot()};
}

SessionPresentation DocumentSimulationSession::Presentation() const
{
    const auto tagged = Snapshot();
    const auto& value = *tagged.publication;
    return {tagged.identity, value.revision, value.state, value.engine.net.marking,
        value.engine.net.transitionStates, value.engine.clock, value.engine.stop,
        value.lastEnd, value.issues, closed_ || value.closed};
}

SessionSnapshot DocumentSimulationSession::WaitForUpdate(std::uint64_t revision,
    std::chrono::milliseconds timeout) const
{
    RequireOwner();
    return {identity_, service_.WaitForUpdate(revision, timeout)};
}

std::shared_future<void> DocumentSimulationSession::Completion() const
{
    RequireOwner(); return service_.Completion();
}

void DocumentSimulationSession::Close()
{
    RequireOwner();
    closed_ = true;
    service_.Shutdown();
}
}
