#include "stdafx.h"
#include "PetriSimDoc.h"
#include "resource.h"
#include "adapters/LegacyDocumentSessionBinding.h"
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace hps::application;
using namespace hps::simulation;
using hps::adapters::LegacyDocumentSessionBinding;
using namespace std::chrono_literals;
namespace
{
class StatusWindow : public CWnd
{
public:
    StatusWindow()
    {
        if (!CreateEx(0, AfxRegisterWndClass(0), L"Binding status tests", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr))
            throw std::runtime_error("Cannot create binding status window");
        previous_ = AfxGetThread()->m_pMainWnd;
        AfxGetThread()->m_pMainWnd = this;
    }
    ~StatusWindow() override
    {
        AfxGetThread()->m_pMainWnd = previous_;
        CHECK(DestroyWindow() != FALSE);
    }
private:
    CWnd* previous_ = nullptr;
};
class Document : public CPetriSimDoc
{
public:
    using CPetriSimDoc::OnSimCommand;
    Document() { m_bAutoDelete = FALSE; DeleteContents(); }
    ~Document() override { DeleteContents(); }
    template<class Object> Object& Add()
    {
        auto object = std::make_unique<Object>(CPoint(0, 0), 1);
        CHDrawObjectList list; list.AddTail(object.get());
        AddObjects(list);
        return *object.release(); // Document now owns the registered object.
    }
    void Net()
    {
        auto& place = Add<CHPosition>(); place.SetTokenMax(10); place.SetToken(8);
        auto& transition = Add<CHTransition>();
        transition.TimeMode(CHTransition::TTM_EQUAL_DISTR);
        transition.SetStartDelay(2); transition.SetRangeDelay(7); transition.SetDelay(0);
        auto& input = Add<CHConnector>(); input.From(&place); input.To(&transition);
        auto& output = Add<CHConnector>(); output.From(&transition); output.To(&place);
    }
};
CommandReply Command(LegacyDocumentSessionBinding& binding, ServiceCommand command)
{
    auto pending = binding.Submit(command);
    REQUIRE(pending.completion.wait_for(5s) == std::future_status::ready);
    return pending.completion.get();
}
void Closed(LegacyDocumentSessionBinding& binding, SessionIdentity old)
{
    CHECK_FALSE(binding.Accepts(old));
    REQUIRE(binding.Completion().valid());
    CHECK(binding.Completion().wait_for(0ms) == std::future_status::ready);
    binding.Completion().get();
    const auto snapshot = binding.Snapshot();
    if (snapshot) CHECK(snapshot->publication->closed);
}
}

TEST_CASE("Lifecycle observers see object lists before addition and removal", "[session-binding]")
{
    struct Observer : CHDocumentSessionLifecycle
    {
        Document& document;
        std::vector<INT_PTR> counts;
        explicit Observer(Document& value) : document(value) {}
        void BeforeDocumentChange(DocumentSessionChange) override { counts.push_back(document.DrawObjectList().GetCount()); }
        void DocumentDestroyed() noexcept override {}
    };
    Document document;
    auto observer = std::make_shared<Observer>(document);
    REQUIRE(document.AttachSessionLifecycle(observer));
    auto& object = document.Add<CHPosition>();
    REQUIRE(observer->counts.size() >= 2);
    CHECK(observer->counts.front() == 0); // Before ID assignment/list insertion.
    observer->counts.clear();
    CHDrawObjectList remove; remove.AddTail(&object);
    document.RemoveObjects(remove);
    REQUIRE_FALSE(observer->counts.empty()); CHECK(observer->counts.front() == 1);
    document.DetachSessionLifecycle(*observer);
}
TEST_CASE("A document has at most one weak binding and can attach again after release", "[session-binding]")
{
    Document document; document.Net();
    {
        auto binding = LegacyDocumentSessionBinding::Attach(document, DocumentId{1});
        REQUIRE(binding->Start().empty());
        CHECK_THROWS_AS(LegacyDocumentSessionBinding::Attach(document, DocumentId{1}), std::logic_error);
        CHECK(binding->Accepts(*binding->Identity()));
    }
    auto replacement = LegacyDocumentSessionBinding::Attach(document, DocumentId{1});
    CHECK(replacement->Start().empty());
}
TEST_CASE("Dirty notification invalidates even an already modified document and restart changes identity", "[session-binding]")
{
    Document document; document.Net();
    REQUIRE(document.IsModified());
    auto binding = LegacyDocumentSessionBinding::Attach(document, DocumentId{2});
    REQUIRE(binding->Start().empty());
    const auto old = *binding->Identity();
    auto pending = binding->Submit(ServiceCommand::Resume);
    document.SetModifiedFlag(TRUE);
    Closed(*binding, old); CHECK(binding->LastChange() == DocumentSessionChange::Edit);
    REQUIRE(pending.completion.wait_for(0ms) == std::future_status::ready); pending.completion.get();
    REQUIRE(binding->Start(7).empty());
    CHECK_FALSE(binding->Accepts(pending.identity)); CHECK(binding->Accepts(*binding->Identity()));
    CHECK(binding->Identity()->session != old.session);
}
TEST_CASE("Clearing dirty state and empty list operations preserve an active binding", "[session-binding]")
{
    Document document; document.Net();
    auto binding = LegacyDocumentSessionBinding::Attach(document, DocumentId{3});
    REQUIRE(binding->Start().empty());
    const auto identity = *binding->Identity();
    document.SetModifiedFlag(FALSE);
    CHDrawObjectList empty; document.AddObjects(empty); document.RemoveObjects(empty);
    CHECK(binding->Accepts(identity)); CHECK_FALSE(binding->LastChange().has_value());
}
TEST_CASE("Actual document additions and removals close the bound service", "[session-binding]")
{
    Document document; document.Net();
    auto binding = LegacyDocumentSessionBinding::Attach(document, DocumentId{4});
    REQUIRE(binding->Start().empty());
    auto old = *binding->Identity();
    auto& isolated = document.Add<CHPosition>();
    Closed(*binding, old);
    REQUIRE(binding->Start().empty()); old = *binding->Identity();
    CHDrawObjectList remove; remove.AddTail(&isolated);
    document.RemoveObjects(remove);
    Closed(*binding, old);
}
TEST_CASE("DeleteContents joins the service before deleting graph objects", "[session-binding]")
{
    Document document; document.Net();
    auto binding = LegacyDocumentSessionBinding::Attach(document, DocumentId{5});
    REQUIRE(binding->Start().empty());
    const auto old = *binding->Identity();
    Command(*binding, ServiceCommand::Resume);
    document.DeleteContents();
    Closed(*binding, old);
    CHECK(document.DrawObjectList().IsEmpty()); CHECK(binding->LastChange() == DocumentSessionChange::Replace);
    CHECK(binding->HasDocument());
}
TEST_CASE("OnCloseDocument invokes close invalidation before base cleanup", "[session-binding]")
{
    Document document; document.Net();
    auto binding = LegacyDocumentSessionBinding::Attach(document, DocumentId{6});
    REQUIRE(binding->Start().empty()); const auto old = *binding->Identity();
    Command(*binding, ServiceCommand::Resume);
    document.OnCloseDocument(); // Test document explicitly disables MFC auto-delete.
    Closed(*binding, old);
    CHECK(binding->LastChange() == DocumentSessionChange::Close);
    CHECK(document.DrawObjectList().IsEmpty());
}
TEST_CASE("Saving preserves a session while archive loading invalidates before replacement", "[session-binding]")
{
    Document document; document.Net();
    auto binding = LegacyDocumentSessionBinding::Attach(document, DocumentId{7});
    REQUIRE(binding->Start().empty()); const auto old = *binding->Identity();
    CMemFile bytes;
    {
        CArchive store(&bytes, CArchive::store); store.m_pDocument = &document;
        document.Serialize(store); store.Close();
    }
    CHECK(binding->Accepts(old));
    document.DeleteContents();
    REQUIRE(binding->Start().empty()); const auto empty = *binding->Identity();
    bytes.SeekToBegin();
    {
        CArchive load(&bytes, CArchive::load); load.m_pDocument = &document;
        document.Serialize(load); load.Close();
    }
    Closed(*binding, empty);
    CHECK(binding->LastChange() == DocumentSessionChange::Replace);
    CHECK(document.DrawObjectList().GetCount() == 4);
    CHECK(binding->Start().empty()); // Same archive model can be exported again.
}
TEST_CASE("Multiple documents invalidate independently", "[session-binding]")
{
    Document first, second; first.Net(); second.Net();
    auto a = LegacyDocumentSessionBinding::Attach(first, DocumentId{8});
    auto b = LegacyDocumentSessionBinding::Attach(second, DocumentId{9});
    REQUIRE(a->Start().empty()); REQUIRE(b->Start().empty());
    const auto oldA = *a->Identity(), oldB = *b->Identity();
    first.BeforeNetworkChange();
    Closed(*a, oldA);
    CHECK(b->Accepts(oldB)); CHECK_FALSE(b->Accepts(oldA));
    second.BeforeNetworkChange(DocumentSessionChange::Replace);
    Closed(*b, oldB);
}
TEST_CASE("Binding safely outlives the destroyed document and detaches without dangling calls", "[session-binding]")
{
    auto document = std::make_unique<Document>(); document->Net();
    auto binding = LegacyDocumentSessionBinding::Attach(*document, DocumentId{10});
    REQUIRE(binding->Start().empty()); const auto old = *binding->Identity();
    Command(*binding, ServiceCommand::Resume);
    document.reset();
    CHECK_FALSE(binding->HasDocument()); CHECK_FALSE(binding->Snapshot().has_value());
    Closed(*binding, old);
    CHECK_THROWS_AS(binding->Start(), std::logic_error);
    binding->Detach();
}
TEST_CASE("Explicit detach and wrong-thread registration do not leave a document observer", "[session-binding]")
{
    Document document; document.Net();
    auto binding = LegacyDocumentSessionBinding::Attach(document, DocumentId{11});
    REQUIRE(binding->Start().empty()); const auto identity = *binding->Identity();
    auto wrong = std::async(std::launch::async, [&] {
        auto rejects = [](auto action) {
            try { action(); return false; } catch (const std::logic_error&) { return true; }
        };
        return rejects([&] { binding->Start(); }) &&
            rejects([&] { document.BeforeNetworkChange(); }) &&
            rejects([&] { LegacyDocumentSessionBinding::Attach(document, DocumentId{12}); });
    });
    CHECK(wrong.get()); CHECK(binding->Accepts(identity));
    binding->Detach(); Closed(*binding, identity); CHECK_FALSE(binding->HasDocument());
    document.BeforeNetworkChange();
    auto replacement = LegacyDocumentSessionBinding::Attach(document, DocumentId{11});
    CHECK(replacement->Start().empty());
}
TEST_CASE("Legacy simulation commands invalidate the new service before document reset", "[session-binding]")
{
    StatusWindow status; // Legacy command sends StatusBar even while offline.
    Document document; document.Net();
    auto binding = LegacyDocumentSessionBinding::Attach(document, DocumentId{13});
    REQUIRE(binding->Start().empty()); const auto old = *binding->Identity();
    document.OnSimCommand(ID_SIM_RESET);
    Closed(*binding, old);
    CHECK(binding->LastChange() == DocumentSessionChange::LegacySimulation);
    CHECK_FALSE(document.SimOnline());
}
