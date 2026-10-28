#pragma once
#include "LegacySessionAdapter.h"
#include "hpetrisim/HDocumentSessionLifecycle.h"

namespace hps::adapters
{
// Opt-in owner-thread binding. Document borrows a weak observer; binding owns
// the value session. Neither worker nor session retains a document pointer.
class LegacyDocumentSessionBinding final : public CHDocumentSessionLifecycle
{
public:
    static std::shared_ptr<LegacyDocumentSessionBinding> Attach(CPetriSimDoc& document,
        application::DocumentId identity);
    ~LegacyDocumentSessionBinding() override;
    LegacyDocumentSessionBinding(const LegacyDocumentSessionBinding&) = delete;
    LegacyDocumentSessionBinding& operator=(const LegacyDocumentSessionBinding&) = delete;

    std::vector<domain::ValidationIssue> Start(std::uint32_t seed = 1);
    application::SessionCommand Submit(simulation::ServiceCommand command);
    std::optional<application::SessionSnapshot> Snapshot() const;
    std::optional<application::SessionIdentity> Identity() const;
    bool Accepts(application::SessionIdentity identity) const;
    bool HasDocument() const;
    std::optional<DocumentSessionChange> LastChange() const;
    std::shared_future<void> Completion() const;
    void Close();
    void Detach();

    void BeforeDocumentChange(DocumentSessionChange reason) override;
    void DocumentDestroyed() noexcept override;

private:
    explicit LegacyDocumentSessionBinding(application::DocumentId identity);
    void RequireOwner() const;

    CPetriSimDoc* document_ = nullptr; // Invalidated by DocumentDestroyed before document dies.
    application::DocumentId documentId_;
    std::thread::id owner_;
    std::unique_ptr<application::DocumentSimulationSession> session_;
    std::optional<application::SessionIdentity> identity_;
    std::optional<DocumentSessionChange> lastChange_;
    std::shared_future<void> completion_;
};
}
