#pragma once
#include <memory>

enum class DocumentSessionChange { Edit, Replace, Close, LegacySimulation };

// Optional owner-thread observer. Document holds only a weak reference.
class CHDocumentSessionLifecycle
{
public:
    virtual ~CHDocumentSessionLifecycle() = default;
    virtual void BeforeDocumentChange(DocumentSessionChange reason) = 0;
    virtual void DocumentDestroyed() noexcept = 0;
};
