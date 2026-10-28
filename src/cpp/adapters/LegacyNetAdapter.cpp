#include "stdafx.h"
#include "PetriSimDoc.h"
#include "LegacyNetAdapter.h"
#include <set>
#include <unordered_map>

namespace hps::adapters
{
namespace
{
std::u16string CopyName(const CString& source)
{
    std::u16string result;
    result.reserve(source.GetLength());
    for (int index = 0; index < source.GetLength(); ++index)
        result.push_back(static_cast<char16_t>(source[index]));
    return result;
}
std::uint64_t LegacyUnsigned(long value)
{
    // The legacy fields are ULONG32 even where their getters return long.
    return static_cast<std::uint32_t>(value);
}
}

ExportResult ExportLegacyNet(CPetriSimDoc& document)
{
    using namespace domain;
    if (document.SimOnline())
        return std::vector<ValidationIssue>{{ValidationCode::SnapshotUnavailable, 0, "Take the simulation offline before exporting a snapshot"}};
    NetSnapshot snapshot;
    std::vector<ValidationIssue> issues;
    std::set<std::uint32_t> ids;
    std::unordered_map<CHNetMember*, NodeId> nodes;
    auto& objects = document.DrawObjectList();
    for (POSITION position = objects.GetHeadPosition(); position;)
    {
        auto* object = objects.GetNext(position);
        if (!object)
        {
            issues.push_back({ValidationCode::InvalidObject, 0, "Null object in legacy document"});
            continue;
        }
        const bool networkObject = dynamic_cast<CHNetMember*>(object) || dynamic_cast<CHConnector*>(object);
        if (networkObject && !ids.insert(object->Id()).second)
            issues.push_back({ValidationCode::DuplicateId, object->Id(), "Duplicate legacy network ID"});
        if (auto* place = dynamic_cast<CHPosition*>(object))
        {
            const PlaceId id{place->Id()};
            snapshot.net.places.push_back({id, CopyName(place->Name()), LegacyUnsigned(place->GetTokenMax()), place->InitialTokens()});
            snapshot.marking.emplace(id, PlaceState{LegacyUnsigned(place->GetToken()), LegacyUnsigned(place->GetTokenCount())});
            nodes.emplace(place, id);
        }
        else if (auto* transition = dynamic_cast<CHTransition*>(object))
        {
            TimeMode mode{};
            switch (transition->TimeMode())
            {
            case CHTransition::TTM_IMMIDIATE: mode = TimeMode::Immediate; break;
            case CHTransition::TTM_DELAY: mode = TimeMode::FixedDelay; break;
            case CHTransition::TTM_EXPONENTIAL: mode = TimeMode::Exponential; break;
            case CHTransition::TTM_EQUAL_DISTR: mode = TimeMode::Uniform; break;
            default:
                issues.push_back({ValidationCode::InvalidEnum, transition->Id(), "Unknown legacy time mode"});
                continue;
            }
            const TransitionId id{transition->Id()};
            snapshot.net.transitions.push_back({id, CopyName(transition->Name()), mode,
                static_cast<std::int64_t>(LegacyUnsigned(transition->GetStartDelay())),
                static_cast<std::int64_t>(LegacyUnsigned(transition->GetRangeDelay()))});
            snapshot.transitionStates.emplace(id, TransitionState{transition->GetDelay(),
                LegacyUnsigned(transition->GetTokensCount()), transition->Activ(), transition->Enabled()});
            nodes.emplace(transition, id);
        }
    }
    for (POSITION position = objects.GetHeadPosition(); position;)
    {
        auto* arc = dynamic_cast<CHConnector*>(objects.GetNext(position));
        if (!arc) continue; // Drawing objects are outside the domain projection.
        if (!arc->Connected())
        {
            issues.push_back({ValidationCode::DisconnectedArc, arc->Id(), "Legacy arc has an unconnected endpoint"});
            continue;
        }
        auto from = nodes.find(arc->From());
        auto to = nodes.find(arc->To());
        if (from == nodes.end() || to == nodes.end())
        {
            issues.push_back({ValidationCode::ForeignEndpoint, arc->Id(), "Arc points outside the exported document"});
            continue;
        }
        const auto fromId = std::visit([](auto id) { return id.value; }, from->second);
        const auto toId = std::visit([](auto id) { return id.value; }, to->second);
        if (fromId != arc->FromId() || toId != arc->ToId())
            issues.push_back({ValidationCode::StaleReference, arc->Id(), "Arc endpoint IDs disagree with their objects"});
        ArcKind kind{};
        switch (arc->Type())
        {
        case CHConnector::TypeNormal: kind = ArcKind::Normal; break;
        case CHConnector::TypeInhibitor: kind = ArcKind::Inhibitor; break;
        case CHConnector::TypeTest: kind = ArcKind::Test; break;
        default:
            issues.push_back({ValidationCode::InvalidEnum, arc->Id(), "Unknown legacy arc kind"});
            continue;
        }
        snapshot.net.arcs.push_back({ArcId{arc->Id()}, CopyName(arc->Name()), from->second, to->second, kind, LegacyUnsigned(arc->Weight())});
    }
    auto validation = Validate(snapshot);
    issues.insert(issues.end(), validation.begin(), validation.end());
    if (!issues.empty()) return issues;
    return snapshot;
}
}
