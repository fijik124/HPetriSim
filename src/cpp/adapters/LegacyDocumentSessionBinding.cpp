#include "stdafx.h"
#include "PetriSimDoc.h"
#include "LegacyDocumentSessionBinding.h"
#include <stdexcept>
#include <utility>

namespace hps::adapters
{
LegacyDocumentSessionBinding::LegacyDocumentSessionBinding(application::DocumentId identity)
    : documentId_(identity), owner_(std::this_thread::get_id()) {}

std::shared_ptr<LegacyDocumentSessionBinding> LegacyDocumentSessionBinding::Attach(
    CPetriSimDoc& document, application::DocumentId identity)
{
    if (identity.value == 0) throw std::invalid_argument("Document ID must be nonzero");
    auto binding = std::shared_ptr<LegacyDocumentSessionBinding>(new LegacyDocumentSessionBinding(identity));
    if (!document.AttachSessionLifecycle(binding))
        throw std::logic_error("Document already has a simulation lifecycle binding");
    binding->document_ = &document;
    return binding;
}

LegacyDocumentSessionBinding::~LegacyDocumentSessionBinding()
{
    if (document_) document_->DetachSessionLifecycle(*this);
    // session_ destruction joins without throwing. Completion remains available
    // to any saved future; explicit Close is the reporting boundary.
}

void LegacyDocumentSessionBinding::RequireOwner() const
{
    if (std::this_thread::get_id() != owner_)
        throw std::logic_error("Document session binding requires its owner thread");
}

std::vector<domain::ValidationIssue> LegacyDocumentSessionBinding::Start(std::uint32_t seed)
{
    RequireOwner();
    if (!document_) throw std::logic_error("Document session binding is detached");
    Close();
    auto created = CreateLegacyDocumentSession(*document_, documentId_, seed);
    if (auto* issues = std::get_if<std::vector<domain::ValidationIssue>>(&created)) return std::move(*issues);
    session_ = std::get<std::unique_ptr<application::DocumentSimulationSession>>(std::move(created));
    identity_ = session_->Identity();
    completion_ = session_->Completion();
    lastChange_.reset();
    return {};
}

application::SessionCommand LegacyDocumentSessionBinding::Submit(simulation::ServiceCommand command)
{
    RequireOwner();
    if (!document_ || !session_) throw std::logic_error("No active document simulation session");
    return session_->Submit(command);
}

std::optional<application::SessionSnapshot> LegacyDocumentSessionBinding::Snapshot() const
{
    RequireOwner();
    if (!session_) return std::nullopt;
    return session_->Snapshot();
}
std::optional<application::SessionIdentity> LegacyDocumentSessionBinding::Identity() const
{
    RequireOwner(); return identity_;
}
bool LegacyDocumentSessionBinding::Accepts(application::SessionIdentity identity) const
{
    RequireOwner(); return document_ && session_ && session_->Accepts(identity);
}
bool LegacyDocumentSessionBinding::HasDocument() const { RequireOwner(); return document_ != nullptr; }
std::optional<DocumentSessionChange> LegacyDocumentSessionBinding::LastChange() const
{
    RequireOwner(); return lastChange_;
}
std::shared_future<void> LegacyDocumentSessionBinding::Completion() const
{
    RequireOwner(); return completion_;
}
void LegacyDocumentSessionBinding::Close()
{
    RequireOwner();
    if (session_) session_->Close();
}
void LegacyDocumentSessionBinding::Detach()
{
    RequireOwner();
    Close();
    if (document_) document_->DetachSessionLifecycle(*this);
    document_ = nullptr;
}
void LegacyDocumentSessionBinding::BeforeDocumentChange(DocumentSessionChange reason)
{
    RequireOwner();
    if (!lastChange_) lastChange_ = reason;
    Close(); // Session invalidates its identity before joining.
}
void LegacyDocumentSessionBinding::DocumentDestroyed() noexcept
{
    document_ = nullptr;
    lastChange_ = DocumentSessionChange::Close;
    session_.reset();
}
}
