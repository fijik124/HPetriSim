#pragma once
#include "simulation/BackgroundSimulation.h"

namespace hps::application
{
struct DocumentId
{
    std::uint64_t value = 0;
    auto operator<=>(const DocumentId&) const = default;
};
struct SessionId
{
    std::uint64_t value = 0;
    auto operator<=>(const SessionId&) const = default;
};
struct SessionIdentity
{
    DocumentId document;
    SessionId session;
    auto operator<=>(const SessionIdentity&) const = default;
};
struct SessionOptions
{
    domain::StepClock clock;
    std::uint32_t seed = 1;
    std::size_t batchSize = 16;
    std::size_t queueCapacity = 64;
};
struct SessionSnapshot
{
    SessionIdentity identity;
    std::shared_ptr<const simulation::PublishedSimulation> publication;
};
struct SessionCommand
{
    SessionIdentity identity;
    std::future<simulation::CommandReply> completion;
};
struct SessionPresentation
{
    SessionIdentity identity;
    std::uint64_t revision;
    simulation::RunState state;
    domain::Marking marking;
    std::map<domain::TransitionId, domain::TransitionState> transitions;
    domain::StepClock clock;
    domain::StopFlags stop;
    std::optional<simulation::RunEndReason> lastEnd;
    std::vector<domain::ValidationIssue> issues;
    bool closed;
};

// Public methods belong to the creating thread. Worker state contains only
// copied values; no document/window pointer or frontend callback is retained.
class DocumentSimulationSession
{
public:
    static std::variant<std::unique_ptr<DocumentSimulationSession>, std::vector<domain::ValidationIssue>> Create(
        DocumentId document, const domain::NetSnapshot& snapshot,
        const domain::ConflictTopology& topology, const SessionOptions& options = {});
    ~DocumentSimulationSession() = default;
    DocumentSimulationSession(const DocumentSimulationSession&) = delete;
    DocumentSimulationSession& operator=(const DocumentSimulationSession&) = delete;
    DocumentSimulationSession(DocumentSimulationSession&&) = delete;
    DocumentSimulationSession& operator=(DocumentSimulationSession&&) = delete;

    SessionIdentity Identity() const;
    bool Accepts(SessionIdentity identity) const;
    bool IsClosed() const;
    SessionCommand Submit(simulation::ServiceCommand command);
    SessionSnapshot Snapshot() const;
    SessionPresentation Presentation() const;
    SessionSnapshot WaitForUpdate(std::uint64_t revision, std::chrono::milliseconds timeout) const;
    std::shared_future<void> Completion() const;
    // Invalidates identity acceptance before joining. Rethrows service failures.
    void Close();

private:
    DocumentSimulationSession(SessionIdentity identity, simulation::SimulationEngine engine,
        const SessionOptions& options);
    void RequireOwner() const;

    SessionIdentity identity_;
    std::thread::id owner_;
    bool closed_ = false;
    simulation::BackgroundSimulation service_;
};
using SessionCreationResult = std::variant<std::unique_ptr<DocumentSimulationSession>, std::vector<domain::ValidationIssue>>;
}
