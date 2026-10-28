#include "stdafx.h"
#include "PetriSimDoc.h"
#include "resource.h"
#include "adapters/LegacyNetAdapter.h"
#include "domain/SimulationRules.h"
#include "domain/TokenEffects.h"
#include "domain/Timing.h"
#include "domain/ConflictSelection.h"
#include "domain/ConflictScan.h"
#include "domain/SimulationStep.h"
#include "simulation/SimulationEngine.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
class SimulationMessages : public CWnd
{
public:
	explicit SimulationMessages(CHSimulation& simulation) : simulation_(simulation)
	{
		if (!CreateEx(0, AfxRegisterWndClass(0), L"HPetriSim simulation tests", 0,
			0, 0, 0, 0, HWND_MESSAGE, nullptr))
		{
			throw std::runtime_error("Cannot create simulation message window");
		}
	}
	~SimulationMessages() override
	{
		CHECK(DestroyWindow() != FALSE);
	}
	std::vector<CHSimulation::UpdateMode> updates;

protected:
	LRESULT WindowProc(UINT message, WPARAM wParam, LPARAM lParam) override
	{
		if (message == WM_SIMTHREAD)
		{
			updates.push_back(static_cast<CHSimulation::UpdateMode>(wParam));
			// Use the real UI-message handler (including clearing invalid flags),
			// with no views, output pane or main frame attached to the document.
			simulation_.OnSimThreadMessage(wParam, lParam);
			return 0;
		}
		return CWnd::WindowProc(message, wParam, lParam);
	}

private:
	CHSimulation& simulation_;
};
}

// Sole friend of CHSimulation. Owns every model object; legacy collections only
// borrow them. Online()/Run()/Shutdown() are intentionally never invoked.
class CHSimulationCharacterization
{
public:
	CHSimulationCharacterization() : messages(document.Sim())
	{
		document.Sim().m_hWnd = messages.GetSafeHwnd();
		document.Sim().SimSpeed(1); // Normal mode without timer-driven animation.
	}
	~CHSimulationCharacterization()
	{
		for (const auto& arc : arcs)
		{
			arc->From(nullptr);
			arc->To(nullptr);
		}
		document.Sim().m_Positions.RemoveAll();
		document.Sim().m_Transitions.RemoveAll();
		document.Sim().m_ConflictedTransitions.RemoveAll();
		document.Sim().m_ConnectorsAnimatedIn.RemoveAll();
		document.Sim().m_ConnectorsAnimatedOut.RemoveAll();
		document.DrawObjectList().RemoveAll(); // Aliases, owned by vectors below.
	}
	CHPosition& Place(long tokens, long capacity = 100)
	{
		std::unique_ptr<CHPosition> object(new CHPosition(CPoint(0, 0), ++nextId));
		object->Id(nextId);
		object->SetToken(tokens);
		object->SetTokenMax(capacity);
		CHPosition& result = *object;
		places.push_back(std::move(object));
		document.Sim().m_Positions.Add(&result);
		document.DrawObjectList().AddTail(&result);
		return result;
	}
	CHTransition& Transition(CHTransition::TransitionTimeMode mode = CHTransition::TTM_IMMIDIATE,
		long delay = 0, long range = 0)
	{
		std::unique_ptr<CHTransition> object(new CHTransition(CPoint(50, 0), ++nextId));
		object->Id(nextId);
		object->TimeMode(mode);
		object->SetStartDelay(delay);
		object->SetDelay(delay);
		object->SetRangeDelay(range);
		CHTransition& result = *object;
		transitions.push_back(std::move(object));
		document.Sim().m_Transitions.Add(&result);
		document.DrawObjectList().AddTail(&result);
		return result;
	}
	CHConnector& Arc(CHNetMember& from, CHNetMember& to, long weight = 1,
		CHConnector::ConnectorType type = CHConnector::TypeNormal)
	{
		std::unique_ptr<CHConnector> object(new CHConnector(CPoint(0, 0), ++nextId));
		object->Id(nextId);
		object->Weigth(weight);
		object->Type(type);
		object->From(&from);
		object->To(&to);
		CHConnector& result = *object;
		arcs.push_back(std::move(object));
		document.DrawObjectList().AddTail(&result);
		return result;
	}
	bool Step(bool fast = true)
	{
		using namespace hps::domain;
		const auto exported = hps::adapters::ExportLegacyNet(document);
		REQUIRE(std::holds_alternative<NetSnapshot>(exported));
		const auto evaluated = EvaluateLegacyEligibility(std::get<NetSnapshot>(exported));
		REQUIRE(std::holds_alternative<std::vector<TransitionEligibility>>(evaluated));
		const auto& expected = std::get<std::vector<TransitionEligibility>>(evaluated);
		const StepClock clockBefore{document.Sim().m_SimTime, document.Sim().m_StepCount,
			document.Sim().m_SampleTime, document.Sim().m_TimeLimit, document.Sim().m_StepLimit};
		bool locked = false;
		REQUIRE(document.Sim().SingleStep(fast, locked));
		REQUIRE(expected.size() == static_cast<std::size_t>(document.Sim().m_Transitions.GetCount()));
		for (std::size_t index = 0; index < expected.size(); ++index)
		{
			auto& actual = *document.Sim().m_Transitions[static_cast<INT_PTR>(index)];
			REQUIRE(expected[index].id.value == actual.Id());
			const auto& state = expected[index].state;
			if (std::holds_alternative<ArcBlocked>(state))
			{
				CHECK_FALSE(actual.Activ());
				CHECK_FALSE(actual.Enabled());
			}
			else if (std::holds_alternative<DelayPending>(state))
			{
				CHECK_FALSE(actual.Activ());
				CHECK(actual.Enabled());
			}
			else
			{
				CHECK_FALSE(actual.Enabled());
				// The pure evaluator stops before conflict selection. Candidates
				// can be rejected by the real legacy conflict pass afterwards.
				if (document.Sim().m_ConflictedTransitions.IsEmpty()) CHECK(actual.Activ());
			}
		}
		std::vector<TransitionId> selected;
		for (INT_PTR index = 0; index < document.Sim().m_Transitions.GetCount(); ++index)
			if (document.Sim().m_Transitions[index]->Activ())
				selected.push_back(TransitionId{document.Sim().m_Transitions[index]->Id()});
		// Selection is supplied by the actual legacy conflict pass; only token
		// effects are predicted independently from the pre-step snapshot.
		const auto effect = PlanLegacyTokenEffects(std::get<NetSnapshot>(exported), selected);
		REQUIRE(std::holds_alternative<TokenEffectPlan>(effect));
		const auto& plan = std::get<TokenEffectPlan>(effect);
		for (INT_PTR index = 0; index < document.Sim().m_Positions.GetCount(); ++index)
		{
			auto& actual = *document.Sim().m_Positions[index];
			const auto& predicted = plan.markingAfter.at(PlaceId{actual.Id()});
			CHECK(predicted.tokens == static_cast<std::uint32_t>(actual.GetToken()));
			CHECK(predicted.producedTokens == static_cast<std::uint32_t>(actual.GetTokenCount()));
		}
		for (INT_PTR index = 0; index < document.Sim().m_Transitions.GetCount(); ++index)
		{
			auto& actual = *document.Sim().m_Transitions[index];
			CHECK(plan.consumedTokensAfter.at(TransitionId{actual.Id()}) == static_cast<std::uint32_t>(actual.GetTokensCount()));
		}
		SampledDelays samples;
		for (INT_PTR index = 0; index < document.Sim().m_Transitions.GetCount(); ++index)
		{
			auto& actual = *document.Sim().m_Transitions[index];
			if (actual.Activ() && (actual.TimeMode() == CHTransition::TTM_EXPONENTIAL || actual.TimeMode() == CHTransition::TTM_EQUAL_DISTR))
				samples.emplace(TransitionId{actual.Id()}, actual.GetDelay());
		}
		// Legacy supplies only selection and stochastic samples at this boundary.
		// Clock progression, fixed resets and countdown are predicted from before.
		const auto timing = PlanLegacyTiming(std::get<NetSnapshot>(exported), selected, clockBefore, samples);
		REQUIRE(std::holds_alternative<TimingPlan>(timing));
		const auto& timed = std::get<TimingPlan>(timing);
		CHECK(timed.clockAfter.timeMs == document.Sim().m_SimTime);
		CHECK(timed.clockAfter.steps == document.Sim().m_StepCount);
		CHECK(timed.stop.Locked() == locked);
		CHECK(timed.stop.deadlock == Logged(L"Deadlock"));
		for (INT_PTR index = 0; index < document.Sim().m_Transitions.GetCount(); ++index)
		{
			auto& actual = *document.Sim().m_Transitions[index];
			const auto& predicted = timed.transitionsAfter.at(TransitionId{actual.Id()});
			CHECK(predicted.remainingDelayMs == actual.GetDelay());
			CHECK(predicted.active == actual.Activ());
			CHECK(predicted.enabled == actual.Enabled());
		}
		return locked;
	}
	bool CheckSelection(const std::vector<std::int64_t>& choices, bool fast = true)
	{
		using namespace hps::domain;
		const auto exported = hps::adapters::ExportLegacySimulationSnapshot(document);
		REQUIRE(std::holds_alternative<hps::adapters::LegacySimulationSnapshot>(exported));
		const auto& before = std::get<hps::adapters::LegacySimulationSnapshot>(exported);
		std::vector<TransitionId> order;
		for (INT_PTR index = 0; index < document.Sim().m_ConflictedTransitions.GetCount(); ++index)
			order.push_back(TransitionId{document.Sim().m_ConflictedTransitions[index]->Id()});
		// Predict before executing the kernel; no winners are copied as inputs.
		const auto prediction = PlanLegacyConflictSelection(before.snapshot, before.topology, order, choices);
		REQUIRE(std::holds_alternative<ConflictSelectionPlan>(prediction));
		const bool locked = Step(fast);
		std::vector<TransitionId> actual;
		for (INT_PTR index = 0; index < document.Sim().m_Transitions.GetCount(); ++index)
			if (document.Sim().m_Transitions[index]->Activ())
				actual.push_back(TransitionId{document.Sim().m_Transitions[index]->Id()});
		CHECK(actual == std::get<ConflictSelectionPlan>(prediction).selectedInNetOrder);
		return locked;
	}
	std::vector<hps::domain::TransitionId> ScanLegacy()
	{
		document.Sim().m_ConflictedTransitions.RemoveAll();
		document.Sim().ScanPotentialConflicts();
		std::vector<hps::domain::TransitionId> result;
		for (INT_PTR index = 0; index < document.Sim().m_ConflictedTransitions.GetCount(); ++index)
			result.push_back(hps::domain::TransitionId{document.Sim().m_ConflictedTransitions[index]->Id()});
		return result;
	}
	void CheckPreScan(const std::vector<hps::domain::TransitionId>& expected)
	{
		using namespace hps::domain;
		const auto exported = hps::adapters::ExportLegacySimulationSnapshot(document);
		REQUIRE(std::holds_alternative<hps::adapters::LegacySimulationSnapshot>(exported));
		const auto& before = std::get<hps::adapters::LegacySimulationSnapshot>(exported);
		const auto predicted = FindLegacyPotentialConflicts(before.snapshot, before.topology);
		REQUIRE(std::holds_alternative<std::vector<TransitionId>>(predicted));
		CHECK(std::get<std::vector<TransitionId>>(predicted) == expected);
		CHECK(ScanLegacy() == expected);
	}
	hps::domain::StepClock Clock()
	{
		return {document.Sim().m_SimTime, document.Sim().m_StepCount,
			document.Sim().m_SampleTime, document.Sim().m_TimeLimit, document.Sim().m_StepLimit};
	}
	hps::adapters::LegacySimulationSnapshot BeginStepComparison()
	{
		ScanLegacy();
		// Match Online's post-scan preparation once, without creating its worker.
		for (INT_PTR index = 0; index < document.Sim().m_Transitions.GetCount(); ++index)
		{
			document.Sim().m_Transitions[index]->Activ(false);
			document.Sim().m_Transitions[index]->Enabled(false);
		}
		const auto exported = hps::adapters::ExportLegacySimulationSnapshot(document);
		REQUIRE(std::holds_alternative<hps::adapters::LegacySimulationSnapshot>(exported));
		return std::get<hps::adapters::LegacySimulationSnapshot>(exported);
	}
	hps::domain::SimulationStepPlan CheckFullStep(const hps::domain::NetSnapshot& before,
		const hps::domain::ConflictTopology& topology, const hps::domain::StepClock& clock,
		const hps::domain::StepRandomInputs& random = {}, bool fast = true)
	{
		using namespace hps::domain;
		const auto predicted = PlanLegacySimulationStep(before, topology, clock, random);
		REQUIRE(std::holds_alternative<SimulationStepPlan>(predicted));
		const auto& plan = std::get<SimulationStepPlan>(predicted);
		const bool locked = Step(fast);
		const auto exported = hps::adapters::ExportLegacyNet(document);
		REQUIRE(std::holds_alternative<NetSnapshot>(exported));
		const auto& actual = std::get<NetSnapshot>(exported);
		const auto& expected = plan.snapshotAfter;
		REQUIRE(actual.marking.size() == expected.marking.size());
		for (const auto& [id, value] : expected.marking)
		{
			CHECK(value.tokens == actual.marking.at(id).tokens);
			CHECK(value.producedTokens == actual.marking.at(id).producedTokens);
		}
		REQUIRE(actual.transitionStates.size() == expected.transitionStates.size());
		for (const auto& [id, value] : expected.transitionStates)
		{
			const auto& state = actual.transitionStates.at(id);
			CHECK(value.remainingDelayMs == state.remainingDelayMs);
			CHECK(value.consumedTokens == state.consumedTokens);
			CHECK(value.active == state.active);
			CHECK(value.enabled == state.enabled);
		}
		REQUIRE(actual.net.places.size() == expected.net.places.size());
		REQUIRE(actual.net.transitions.size() == expected.net.transitions.size());
		REQUIRE(actual.net.arcs.size() == expected.net.arcs.size());
		for (std::size_t index = 0; index < actual.net.places.size(); ++index)
		{
			const auto& a = actual.net.places[index]; const auto& e = expected.net.places[index];
			CHECK(a.id == e.id); CHECK(a.name == e.name);
			CHECK(a.capacity == e.capacity); CHECK(a.initialTokens == e.initialTokens);
		}
		for (std::size_t index = 0; index < actual.net.transitions.size(); ++index)
		{
			const auto& a = actual.net.transitions[index]; const auto& e = expected.net.transitions[index];
			CHECK(a.id == e.id); CHECK(a.name == e.name); CHECK(a.mode == e.mode);
			CHECK(a.initialDelayMs == e.initialDelayMs); CHECK(a.rangeMaxDelayMs == e.rangeMaxDelayMs);
		}
		for (std::size_t index = 0; index < actual.net.arcs.size(); ++index)
		{
			const auto& a = actual.net.arcs[index]; const auto& e = expected.net.arcs[index];
			CHECK(a.id == e.id); CHECK(a.name == e.name); CHECK(a.from == e.from);
			CHECK(a.to == e.to); CHECK(a.kind == e.kind); CHECK(a.weight == e.weight);
		}
		const auto actualClock = Clock();
		CHECK(plan.clockAfter.timeMs == actualClock.timeMs);
		CHECK(plan.clockAfter.steps == actualClock.steps);
		CHECK(plan.clockAfter.sampleTimeMs == actualClock.sampleTimeMs);
		CHECK(plan.clockAfter.timeLimitMs == actualClock.timeLimitMs);
		CHECK(plan.clockAfter.stepLimit == actualClock.stepLimit);
		CHECK(plan.stop.Locked() == locked);
		CHECK(plan.stop.deadlock == Logged(L"Deadlock"));
		CHECK(plan.stop.timeLimitReached == (actualClock.timeMs > actualClock.timeLimitMs));
		CHECK(plan.stop.stepLimitReached == (actualClock.steps > actualClock.stepLimit));
		std::vector<TransitionId> actualOrder;
		for (INT_PTR index = 0; index < document.Sim().m_ConflictedTransitions.GetCount(); ++index)
			actualOrder.push_back(TransitionId{document.Sim().m_ConflictedTransitions[index]->Id()});
		CHECK(plan.potentialConflicts == actualOrder);
		CHECK(ValidateConflictTopology(expected, topology).empty());
		return plan;
	}
	void CheckEngineState(const hps::simulation::EngineSnapshot& state, bool afterReset = false)
	{
		using namespace hps::domain;
		const auto exported = hps::adapters::ExportLegacyNet(document);
		REQUIRE(std::holds_alternative<NetSnapshot>(exported));
		const auto& actual = std::get<NetSnapshot>(exported);
		REQUIRE(state.net.marking.size() == actual.marking.size());
		for (const auto& [id, value] : actual.marking)
		{
			CHECK(state.net.marking.at(id).tokens == value.tokens);
			CHECK(state.net.marking.at(id).producedTokens == value.producedTokens);
		}
		REQUIRE(state.net.transitionStates.size() == actual.transitionStates.size());
		for (const auto& [id, value] : actual.transitionStates)
		{
			const auto& copied = state.net.transitionStates.at(id);
			CHECK(copied.remainingDelayMs == value.remainingDelayMs);
			CHECK(copied.consumedTokens == value.consumedTokens);
			CHECK(copied.active == value.active); CHECK(copied.enabled == value.enabled);
		}
		const auto clock = Clock();
		CHECK(state.clock.timeMs == clock.timeMs); CHECK(state.clock.steps == clock.steps);
		CHECK(state.clock.sampleTimeMs == clock.sampleTimeMs);
		CHECK(state.clock.timeLimitMs == clock.timeLimitMs); CHECK(state.clock.stepLimit == clock.stepLimit);
		if (afterReset) CHECK_FALSE(state.stop.Locked());
		else
		{
			CHECK(state.stop.deadlock == Logged(L"Deadlock"));
			CHECK(state.stop.timeLimitReached == (clock.timeMs > clock.timeLimitMs));
			CHECK(state.stop.stepLimitReached == (clock.steps > clock.stepLimit));
		}
	}
	void Conflict(CHTransition& transition) { document.Sim().m_ConflictedTransitions.Add(&transition); }
	void Limits(long time, long steps)
	{
		document.Sim().m_TimeLimit = time;
		document.Sim().m_StepLimit = steps;
	}
	long Steps() { return document.Sim().m_StepCount; }
	long Time() { return document.Sim().m_SimTime; }
	bool Logged(LPCTSTR text) { return document.Sim().m_strBuffer.Find(text) >= 0; }
	void Reset() { document.Sim().Reset(); }
	void SampleTime(long time) { document.Sim().SampleTime(time); }
	void Seed(unsigned int seed) { std::srand(seed); }
	template<class Exporter> auto ExportWithOnlineFlag(Exporter exporter)
	{
		// Exercise the adapter's guard without starting an unsafe legacy worker.
		struct RestoreFlag
		{
			bool& flag;
			bool saved;
			~RestoreFlag() { flag = saved; }
		} restore{document.Sim().m_Online, document.Sim().m_Online};
		document.Sim().m_Online = true;
		return exporter(document);
	}

	CPetriSimDoc document;
	SimulationMessages messages;

private:
	ULONG32 nextId = 0;
	std::vector<std::unique_ptr<CHPosition>> places;
	std::vector<std::unique_ptr<CHTransition>> transitions;
	std::vector<std::unique_ptr<CHConnector>> arcs;
};

TEST_CASE("Domain export rejects online state without starting a worker", "[adapter]")
{
	CHSimulationCharacterization net;
	const auto result = net.ExportWithOnlineFlag(hps::adapters::ExportLegacyNet);
	using Issues = std::vector<hps::domain::ValidationIssue>;
	REQUIRE(std::holds_alternative<Issues>(result));
	const auto& issues = std::get<Issues>(result);
	REQUIRE(issues.size() == 1);
	CHECK(issues[0].code == hps::domain::ValidationCode::SnapshotUnavailable);
	CHECK_FALSE(net.document.SimOnline());
	const auto topology = net.ExportWithOnlineFlag(hps::adapters::ExportLegacySimulationSnapshot);
	REQUIRE(std::holds_alternative<Issues>(topology));
	REQUIRE(std::get<Issues>(topology).size() == 1);
	CHECK(std::get<Issues>(topology)[0].code == hps::domain::ValidationCode::SnapshotUnavailable);
	CHECK_FALSE(net.document.SimOnline());
}

TEST_CASE("Normal arcs consume and produce their weights", "[simulation]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(5);
	auto& output = net.Place(1);
	auto& transition = net.Transition();
	net.Arc(input, transition, 2);
	net.Arc(transition, output, 3);
	REQUIRE_FALSE(net.Step());
	CHECK(input.GetToken() == 3);
	CHECK(output.GetToken() == 4);
	CHECK(output.GetTokenCount() == 3);
	CHECK(transition.GetTokensCount() == 2);
	CHECK(net.Time() == 0);
	CHECK(net.Steps() == 1);
}

TEST_CASE("Insufficient input tokens deadlock without modifying marking", "[simulation]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& output = net.Place(0);
	auto& transition = net.Transition();
	net.Arc(input, transition, 2);
	net.Arc(transition, output);
	REQUIRE(net.Step());
	CHECK(input.GetToken() == 1);
	CHECK(output.GetToken() == 0);
	CHECK(net.Time() == 0);
	CHECK(net.Logged(L"Deadlock"));
}

TEST_CASE("Output capacity permits exact fit and rejects overflow", "[simulation]")
{
	for (long outputTokens : {1L, 2L})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(1);
		auto& output = net.Place(outputTokens, 3);
		auto& transition = net.Transition();
		net.Arc(input, transition);
		net.Arc(transition, output, 2);
		CHECK(net.Step() == (outputTokens == 2));
		CHECK(input.GetToken() == (outputTokens == 1 ? 0 : 1));
		CHECK(output.GetToken() == (outputTokens == 1 ? 3 : 2));
	}
}

TEST_CASE("Test input arcs require tokens but do not consume them", "[simulation]")
{
	for (long tokens : {1L, 2L})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(tokens);
		auto& output = net.Place(0);
		auto& transition = net.Transition();
		net.Arc(input, transition, 2, CHConnector::TypeTest);
		net.Arc(transition, output);
		CHECK(net.Step() == (tokens < 2));
		CHECK(input.GetToken() == tokens);
		CHECK(output.GetToken() == (tokens < 2 ? 0 : 1));
		CHECK(transition.GetTokensCount() == 0);
	}
}

TEST_CASE("Inhibitor input arcs use a strict less-than threshold", "[simulation]")
{
	for (long tokens : {0L, 1L, 2L})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(tokens);
		auto& output = net.Place(0);
		auto& transition = net.Transition();
		net.Arc(input, transition, 2, CHConnector::TypeInhibitor);
		net.Arc(transition, output);
		CHECK(net.Step() == (tokens >= 2));
		CHECK(input.GetToken() == tokens);
		CHECK(output.GetToken() == (tokens >= 2 ? 0 : 1));
	}
}

TEST_CASE("Test output arcs check capacity without producing tokens", "[simulation]")
{
	for (long tokens : {1L, 2L})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(1);
		auto& output = net.Place(tokens, 3);
		auto& transition = net.Transition();
		net.Arc(input, transition);
		net.Arc(transition, output, 2, CHConnector::TypeTest);
		CHECK(net.Step() == (tokens == 2));
		CHECK(input.GetToken() == (tokens == 1 ? 0 : 1));
		CHECK(output.GetToken() == tokens);
	}
}

TEST_CASE("Fixed delay counts down before firing on a later step", "[simulation]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& output = net.Place(0);
	auto& transition = net.Transition(CHTransition::TTM_DELAY, 2);
	net.Arc(input, transition);
	net.Arc(transition, output);
	REQUIRE_FALSE(net.Step());
	CHECK(transition.GetDelay() == 1);
	CHECK(net.Time() == 1);
	CHECK(input.GetToken() == 1);
	REQUIRE_FALSE(net.Step());
	CHECK(transition.GetDelay() == 0);
	CHECK(output.GetToken() == 0);
	REQUIRE_FALSE(net.Step());
	CHECK(output.GetToken() == 1);
	CHECK(net.Time() == 2);
	CHECK(transition.GetDelay() == 2);
}

TEST_CASE("A disabled fixed-delay timer restarts at its initial delay", "[simulation]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& transition = net.Transition(CHTransition::TTM_DELAY, 3);
	net.Arc(input, transition);
	REQUIRE_FALSE(net.Step());
	CHECK(transition.GetDelay() == 2);
	input.SetToken(0);
	REQUIRE(net.Step());
	CHECK(transition.GetDelay() == 3);
	CHECK_FALSE(transition.Enabled());
	CHECK(net.Time() == 1);
}

TEST_CASE("Time and step limits stop only after they are exceeded", "[simulation]")
{
	SECTION("Step limit")
	{
		CHSimulationCharacterization net;
		auto& output = net.Place(0);
		auto& transition = net.Transition();
		net.Arc(transition, output);
		net.Limits(100, 1);
		REQUIRE_FALSE(net.Step());
		REQUIRE(net.Step());
		CHECK(output.GetToken() == 2);
		CHECK(net.Logged(L"Step limit"));
	}
	SECTION("Time limit")
	{
		CHSimulationCharacterization net;
		net.Transition(CHTransition::TTM_DELAY, 10);
		net.Limits(1, 100);
		REQUIRE_FALSE(net.Step());
		REQUIRE(net.Step());
		CHECK(net.Time() == 2);
		CHECK(net.Logged(L"Time limit"));
	}
}

TEST_CASE("Normal and fast steps have the same marking without animation", "[simulation]")
{
	for (bool fast : {false, true})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(2);
		auto& output = net.Place(0);
		auto& transition = net.Transition();
		net.Arc(input, transition);
		net.Arc(transition, output);
		REQUIRE_FALSE(net.Step(fast));
		REQUIRE_FALSE(net.Step(fast));
		CHECK(input.GetToken() == 0);
		CHECK(output.GetToken() == 2);
		CHECK(net.Steps() == 2);
		CHECK(net.Time() == 0);
		CHECK_FALSE(net.messages.updates.empty());
	}
}

TEST_CASE("Reset restores initial marking and clears statistics", "[simulation]")
{
	CHSimulationCharacterization net;
	auto& place = net.Place(0);
	CHPropertyValues properties;
	properties.Add(CHPropertyValue(5L, ID_POSITION_TOKEN_INITIAL));
	REQUIRE(place.SetProperties(properties, false));
	place.SetToken(2);
	place.SetTokenCount(9);
	auto& transition = net.Transition();
	transition.SetTokensCount(7);
	net.Reset();
	CHECK(place.GetToken() == 5);
	CHECK(place.GetTokenCount() == 0);
	CHECK(transition.GetTokensCount() == 0);
	CHECK(net.Time() == 0);
	CHECK(net.Steps() == 0);
}

TEST_CASE("Legacy reset preserves remaining delay of a timed transition", "[simulation][legacy-defect]")
{
	CHSimulationCharacterization net;
	auto& transition = net.Transition(CHTransition::TTM_DELAY, 5);
	transition.SetDelay(2);
	transition.Activ(true);
	transition.Enabled(true);
	net.Reset();
	CHECK(transition.GetDelay() == 2);
	CHECK_FALSE(transition.Activ());
	CHECK_FALSE(transition.Enabled());
}

TEST_CASE("Legacy self-loop checks output capacity before input consumption", "[simulation][legacy-defect]")
{
	CHSimulationCharacterization net;
	auto& place = net.Place(1, 1);
	auto& transition = net.Transition();
	net.Arc(place, transition);
	net.Arc(transition, place);
	CHECK(net.Step());
	CHECK(place.GetToken() == 1);
	CHECK_FALSE(transition.Activ());
}

TEST_CASE("Seeded branch conflict selects one transition", "[simulation][random]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& firstOutput = net.Place(0);
	auto& secondOutput = net.Place(0);
	auto& first = net.Transition();
	auto& second = net.Transition();
	net.Arc(input, first);
	net.Arc(input, second);
	net.Arc(first, firstOutput);
	net.Arc(second, secondOutput);
	net.Conflict(first);
	net.Conflict(second);
	net.Seed(1); // Fixed MSVC CRT sequence; applied after all constructors.
	REQUIRE_FALSE(net.Step());
	CHECK(firstOutput.GetToken() == 1);
	CHECK(secondOutput.GetToken() == 0);
	CHECK(input.GetToken() == 0);
}

TEST_CASE("Legacy conflict index can select an inactive first arc", "[simulation][random][legacy-defect]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& full = net.Place(1, 1);
	auto& available = net.Place(0);
	auto& blocked = net.Transition();
	auto& first = net.Transition();
	auto& second = net.Transition();
	net.Arc(input, blocked);
	net.Arc(input, first);
	net.Arc(input, second);
	net.Arc(blocked, full);
	net.Arc(first, available);
	// second has no output, so this case isolates branch conflict selection.
	net.Conflict(blocked);
	net.Conflict(first);
	net.Conflict(second);
	net.Seed(1);
	CHECK(net.Step());
	CHECK(input.GetToken() == 1);
	CHECK(available.GetToken() == 0);
	CHECK(net.Logged(L"Deadlock"));
}

TEST_CASE("Empty nets deadlock on the first counted step", "[simulation]")
{
	CHSimulationCharacterization net;
	CHECK(net.Step());
	CHECK(net.Steps() == 1);
	CHECK(net.Time() == 0);
	CHECK(net.Logged(L"Deadlock"));
}

TEST_CASE("Output inhibitor arcs do not produce tokens even at full capacity", "[simulation][legacy-policy]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& full = net.Place(1, 1);
	auto& transition = net.Transition();
	net.Arc(input, transition);
	net.Arc(transition, full, 1, CHConnector::TypeInhibitor);
	REQUIRE_FALSE(net.Step());
	CHECK(input.GetToken() == 0);
	CHECK(full.GetToken() == 1);
	CHECK(full.GetTokenCount() == 0);
}

TEST_CASE("Sample interval can overshoot a fixed delay", "[simulation][legacy-policy]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& output = net.Place(0);
	auto& transition = net.Transition(CHTransition::TTM_DELAY, 2);
	net.Arc(input, transition);
	net.Arc(transition, output);
	net.SampleTime(3);
	REQUIRE_FALSE(net.Step());
	CHECK(transition.GetDelay() == -1);
	CHECK(net.Time() == 3);
	CHECK(output.GetToken() == 0);
	REQUIRE_FALSE(net.Step());
	CHECK(output.GetToken() == 1);
	CHECK(net.Time() == 3);
	CHECK(transition.GetDelay() == 2);
}

TEST_CASE("Immediate token traffic prevents timed countdown", "[simulation][legacy-policy]")
{
	CHSimulationCharacterization net;
	auto& loop = net.Place(1, 2);
	auto& immediate = net.Transition();
	net.Arc(loop, immediate);
	net.Arc(immediate, loop);
	auto& timedInput = net.Place(1);
	auto& timedOutput = net.Place(0);
	auto& timed = net.Transition(CHTransition::TTM_DELAY, 2);
	net.Arc(timedInput, timed);
	net.Arc(timed, timedOutput);
	for (int step = 0; step < 3; ++step)
	{
		REQUIRE_FALSE(net.Step());
	}
	CHECK(loop.GetToken() == 1);
	CHECK(timed.GetDelay() == 2);
	CHECK(timedOutput.GetToken() == 0);
	CHECK(net.Time() == 0);
}

TEST_CASE("Seeded uniform delays preserve the current MSVC sequence", "[simulation][random]")
{
	for (int replay = 0; replay < 2; ++replay)
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(4);
		auto& output = net.Place(0);
		auto& transition = net.Transition(CHTransition::TTM_EQUAL_DISTR, 2, 7);
		net.Arc(input, transition);
		net.Arc(transition, output);
		net.Seed(1);
		std::vector<long> trace;
		for (int step = 0; step < 4; ++step)
		{
			transition.SetDelay(0); // Isolate resampling after a successful firing.
			REQUIRE_FALSE(net.Step());
			trace.push_back(transition.GetDelay());
		}
		const std::vector<long> expected = {2, 4, 2, 6};
		CHECK(trace == expected);
		CHECK(output.GetToken() == 4);
	}
}

TEST_CASE("Seeded exponential delays preserve the legacy formula", "[simulation][random][legacy-policy]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(4);
	auto& output = net.Place(0);
	auto& transition = net.Transition(CHTransition::TTM_EXPONENTIAL, 10);
	net.Arc(input, transition);
	net.Arc(transition, output);
	net.Seed(1);
	std::vector<long> trace;
	for (int step = 0; step < 4; ++step)
	{
		transition.SetDelay(0);
		REQUIRE_FALSE(net.Step());
		trace.push_back(transition.GetDelay());
	}
	const std::vector<long> expected = {5, 10, 7, 13};
	CHECK(trace == expected);
	CHECK(output.GetToken() == 4);
}

TEST_CASE("Seeded meet conflict respects a shared output capacity", "[simulation][random]")
{
	CHSimulationCharacterization net;
	auto& firstInput = net.Place(1);
	auto& secondInput = net.Place(1);
	auto& output = net.Place(0, 1);
	auto& first = net.Transition();
	auto& second = net.Transition();
	net.Arc(firstInput, first);
	net.Arc(secondInput, second);
	net.Arc(first, output);
	net.Arc(second, output);
	net.Conflict(first);
	net.Conflict(second);
	net.Seed(1);
	REQUIRE_FALSE(net.Step());
	CHECK(output.GetToken() == 1);
	CHECK(firstInput.GetToken() == 0);
	CHECK(secondInput.GetToken() == 1);
}

TEST_CASE("Branch conflict chooses only one transition even with enough input tokens", "[simulation][random][legacy-policy]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(2);
	auto& first = net.Transition();
	auto& second = net.Transition();
	net.Arc(input, first);
	net.Arc(input, second);
	net.Conflict(first);
	net.Conflict(second);
	net.Seed(1);
	REQUIRE_FALSE(net.Step());
	CHECK(input.GetToken() == 1);
	CHECK(first.GetTokensCount() == 1);
	CHECK(second.GetTokensCount() == 0);
}

TEST_CASE("Pure input predicates match legacy over thresholds and delays", "[rules-differential]")
{
	for (auto kind : {CHConnector::TypeNormal, CHConnector::TypeTest, CHConnector::TypeInhibitor})
		for (long tokens = 0; tokens <= 4; ++tokens)
			for (long weight = 1; weight <= 3; ++weight)
				for (long delay : {0L, 2L})
				{
					INFO("input: " << kind << ", tokens=" << tokens << ", weight=" << weight << ", delay=" << delay);
					CHSimulationCharacterization net;
					auto& input = net.Place(tokens);
					auto& output = net.Place(0);
					auto& transition = net.Transition(CHTransition::TTM_DELAY, delay);
					net.Arc(input, transition, weight, kind);
					net.Arc(transition, output);
					net.Step(); // Step compares the pure decision with real legacy flags.
				}
}

TEST_CASE("Pure output predicates match legacy over capacities and delays", "[rules-differential]")
{
	for (auto kind : {CHConnector::TypeNormal, CHConnector::TypeTest, CHConnector::TypeInhibitor})
		for (long tokens = 0; tokens <= 4; ++tokens)
			for (long weight = 1; weight <= 3; ++weight)
				for (long delay : {0L, 2L})
				{
					INFO("output: " << kind << ", tokens=" << tokens << ", weight=" << weight << ", delay=" << delay);
					CHSimulationCharacterization net;
					auto& input = net.Place(1);
					auto& output = net.Place(tokens, 4);
					auto& transition = net.Transition(CHTransition::TTM_DELAY, delay);
					net.Arc(input, transition);
					net.Arc(transition, output, weight, kind);
					net.Step();
				}
}

TEST_CASE("Mixed arc conditions match the legacy conjunction", "[rules-differential]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(3);
	auto& testInput = net.Place(2);
	auto& inhibitor = net.Place(1);
	auto& output = net.Place(0, 3);
	auto& testOutput = net.Place(2, 4);
	auto& full = net.Place(1, 1);
	auto& transition = net.Transition();
	net.Arc(input, transition, 2);
	net.Arc(testInput, transition, 2, CHConnector::TypeTest);
	net.Arc(inhibitor, transition, 2, CHConnector::TypeInhibitor);
	net.Arc(transition, output, 2);
	net.Arc(transition, testOutput, 2, CHConnector::TypeTest);
	net.Arc(transition, full, 1, CHConnector::TypeInhibitor);
	SECTION("All conditions pass") { CHECK_FALSE(net.Step()); }
	SECTION("One test input blocks") { testInput.SetToken(1); CHECK(net.Step()); }
	SECTION("One output blocks") { testOutput.SetToken(3); CHECK(net.Step()); }
}

TEST_CASE("Eligibility is distinct from conflict selection", "[rules-differential]")
{
	using namespace hps::domain;
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& firstOutput = net.Place(0);
	auto& secondOutput = net.Place(0);
	auto& first = net.Transition();
	auto& second = net.Transition();
	net.Arc(input, first);
	net.Arc(input, second);
	net.Arc(first, firstOutput);
	net.Arc(second, secondOutput);
	net.Conflict(first);
	net.Conflict(second);
	const auto exported = hps::adapters::ExportLegacyNet(net.document);
	REQUIRE(std::holds_alternative<NetSnapshot>(exported));
	const auto result = EvaluateLegacyEligibility(std::get<NetSnapshot>(exported));
	REQUIRE(std::holds_alternative<std::vector<TransitionEligibility>>(result));
	const auto& candidates = std::get<std::vector<TransitionEligibility>>(result);
	REQUIRE(candidates.size() == 2);
	CHECK(std::holds_alternative<FireCandidate>(candidates[0].state));
	CHECK(std::holds_alternative<FireCandidate>(candidates[1].state));
	net.Seed(1);
	CHECK_FALSE(net.Step());
	CHECK(firstOutput.GetToken() + secondOutput.GetToken() == 1);
}

TEST_CASE("Legacy signed maximum marking matches the pure decision", "[rules-differential]")
{
	CHSimulationCharacterization net;
	const auto maximum = (std::numeric_limits<long>::max)();
	auto& input = net.Place(maximum, maximum);
	auto& output = net.Place(0);
	auto& transition = net.Transition();
	net.Arc(input, transition);
	net.Arc(transition, output);
	CHECK_FALSE(net.Step());
	CHECK(input.GetToken() == maximum - 1);
	CHECK(output.GetToken() == 1);
}

TEST_CASE("Every legacy timing mode uses the current delay for eligibility", "[rules-differential]")
{
	for (auto mode : {CHTransition::TTM_IMMIDIATE, CHTransition::TTM_DELAY, CHTransition::TTM_EXPONENTIAL, CHTransition::TTM_EQUAL_DISTR})
		for (long delay : {-1L, 0L, 1L, 2L})
		{
			INFO("time mode=" << mode << ", delay=" << delay);
			CHSimulationCharacterization net;
			auto& input = net.Place(1);
			auto& output = net.Place(0);
			auto& transition = net.Transition(mode, 2, 7);
			transition.SetDelay(static_cast<UINT>(delay));
			net.Arc(input, transition);
			net.Arc(transition, output);
			net.Step();
		}
}

TEST_CASE("Chained transitions observe the previous marking until the next step", "[rules-differential]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& intermediate = net.Place(0);
	auto& output = net.Place(0);
	auto& first = net.Transition();
	auto& second = net.Transition();
	net.Arc(input, first);
	net.Arc(first, intermediate);
	net.Arc(intermediate, second);
	net.Arc(second, output);
	REQUIRE_FALSE(net.Step());
	CHECK(first.Activ());
	CHECK_FALSE(second.Activ());
	CHECK(intermediate.GetToken() == 1);
	CHECK(output.GetToken() == 0);
	REQUIRE_FALSE(net.Step());
	CHECK_FALSE(first.Activ());
	CHECK(second.Activ());
	CHECK(output.GetToken() == 1);
}

TEST_CASE("Weighted multi-input and multi-output effects match real legacy steps", "[effects-differential]")
{
	for (bool fast : {false, true})
	{
		CHSimulationCharacterization net;
		auto& firstInput = net.Place(4);
		auto& secondInput = net.Place(5);
		auto& firstOutput = net.Place(1);
		auto& secondOutput = net.Place(2);
		firstInput.SetTokenCount(31);
		firstOutput.SetTokenCount(11);
		secondOutput.SetTokenCount(21);
		auto& transition = net.Transition();
		transition.SetTokensCount(7);
		net.Arc(firstInput, transition, 2);
		net.Arc(secondInput, transition, 3);
		net.Arc(transition, firstOutput, 4);
		net.Arc(transition, secondOutput);
		REQUIRE_FALSE(net.Step(fast));
		CHECK(firstInput.GetToken() == 2);
		CHECK(secondInput.GetToken() == 2);
		CHECK(firstInput.GetTokenCount() == 31);
		CHECK(firstOutput.GetTokenCount() == 15);
		CHECK(secondOutput.GetTokenCount() == 22);
		CHECK(transition.GetTokensCount() == 12);
	}
}

TEST_CASE("Source and sink effects match a joint legacy step", "[effects-differential]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(5);
	auto& output = net.Place(0);
	auto& source = net.Transition();
	auto& sink = net.Transition();
	net.Arc(source, output, 2);
	net.Arc(input, sink, 3);
	REQUIRE_FALSE(net.Step());
	CHECK(input.GetToken() == 2);
	CHECK(output.GetToken() == 2);
	CHECK(source.GetTokensCount() == 0);
	CHECK(sink.GetTokensCount() == 3);
}

TEST_CASE("Explicit safe batch uses separate input and output phases", "[effects-differential]")
{
	CHSimulationCharacterization net;
	auto& shared = net.Place(4, 6);
	shared.SetTokenCount(3);
	auto& first = net.Transition();
	auto& second = net.Transition();
	net.Arc(shared, first, 2);
	net.Arc(first, shared, 2);
	net.Arc(shared, second, 2);
	net.Arc(second, shared, 2);
	// Deliberately bypass Online() conflict preprocessing: this checks only
	// the real token-transfer code for an explicitly selected, safe batch.
	REQUIRE_FALSE(net.Step());
	CHECK(first.Activ());
	CHECK(second.Activ());
	CHECK(shared.GetToken() == 4);
	CHECK(shared.GetTokenCount() == 7);
	CHECK(first.GetTokensCount() == 2);
	CHECK(second.GetTokensCount() == 2);
}

TEST_CASE("Competing fixed timers restart even when one candidate is deselected", "[timing-differential]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& firstOutput = net.Place(0);
	auto& secondOutput = net.Place(0);
	auto& first = net.Transition(CHTransition::TTM_DELAY, 2);
	auto& second = net.Transition(CHTransition::TTM_DELAY, 2);
	net.Arc(input, first);
	net.Arc(input, second);
	net.Arc(first, firstOutput);
	net.Arc(second, secondOutput);
	net.Conflict(first);
	net.Conflict(second);
	REQUIRE_FALSE(net.Step());
	REQUIRE_FALSE(net.Step());
	CHECK(net.Time() == 2);
	net.Seed(1);
	REQUIRE_FALSE(net.Step());
	CHECK(first.Activ());
	CHECK_FALSE(second.Activ());
	CHECK(first.GetDelay() == 2);
	CHECK(second.GetDelay() == 2);
	CHECK(net.Time() == 2);
}

TEST_CASE("Read-only firings let a separate timer count down", "[timing-differential]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& output = net.Place(0);
	auto& read = net.Transition();
	auto& waiting = net.Transition(CHTransition::TTM_DELAY, 3);
	net.Arc(input, read, 1, CHConnector::TypeTest);
	net.Arc(read, output, 1, CHConnector::TypeTest);
	REQUIRE_FALSE(net.Step());
	CHECK(read.Activ());
	CHECK(waiting.GetDelay() == 2);
	CHECK(net.Time() == 1);
	CHECK(input.GetToken() == 1);
	CHECK(output.GetToken() == 0);
}

TEST_CASE("Explicit stochastic replay predicts timing before the legacy step", "[timing-differential]")
{
	using namespace hps::domain;
	for (auto mode : {CHTransition::TTM_EQUAL_DISTR, CHTransition::TTM_EXPONENTIAL})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(4);
		auto& output = net.Place(0);
		auto& transition = net.Transition(mode, mode == CHTransition::TTM_EQUAL_DISTR ? 2 : 10, 7);
		net.Arc(input, transition);
		net.Arc(transition, output);
		net.Seed(1);
		const std::vector<long> expected = mode == CHTransition::TTM_EQUAL_DISTR
			? std::vector<long>{2, 4, 2, 6} : std::vector<long>{5, 10, 7, 13};
		for (const auto sample : expected)
		{
			transition.SetDelay(0);
			const auto exported = hps::adapters::ExportLegacyNet(net.document);
			REQUIRE(std::holds_alternative<NetSnapshot>(exported));
			const TransitionId id{transition.Id()};
			const auto prediction = PlanLegacyTiming(std::get<NetSnapshot>(exported), {id},
				StepClock{net.Time(), net.Steps(), 1, 1000, 1000}, {{id, sample}});
			REQUIRE(std::holds_alternative<TimingPlan>(prediction));
			REQUIRE_FALSE(net.Step());
			const auto& plan = std::get<TimingPlan>(prediction);
			CHECK(plan.transitionsAfter.at(id).remainingDelayMs == transition.GetDelay());
			CHECK(transition.GetDelay() == sample);
			CHECK(plan.clockAfter.timeMs == net.Time());
			CHECK(plan.clockAfter.steps == net.Steps());
		}
	}
}

TEST_CASE("Legacy normal output retains simultaneous stop reasons", "[timing-differential]")
{
	CHSimulationCharacterization net;
	net.document.Sim().SimTime(2);
	net.Limits(1, 0);
	CHECK(net.Step(false));
	CHECK(net.Logged(L"Time limit"));
	CHECK(net.Logged(L"Step limit"));
	CHECK(net.Logged(L"Deadlock"));
	CHECK(net.Time() == 2);
	CHECK(net.Steps() == 1);
}

TEST_CASE("Conflict winners follow seeded draws and connector order", "[conflict-characterization][random]")
{
	// Recorded MSVC first-draw indices for RandLong(0, 2), not a portable RNG contract.
	const unsigned seeds[] = {1, 5000, 5010, 10000};
	const int winners[] = {0, 0, 1, 1};
	for (int sample = 0; sample < 4; ++sample)
		for (bool reverse : {false, true})
		{
			INFO("seed=" << seeds[sample] << ", reverse=" << reverse);
			CHSimulationCharacterization net;
			auto& input = net.Place(2);
			auto& first = net.Transition();
			auto& second = net.Transition();
			if (reverse) { net.Arc(input, second); net.Arc(input, first); }
			else { net.Arc(input, first); net.Arc(input, second); }
			net.Conflict(first);
			net.Conflict(second);
			net.Seed(seeds[sample]);
			CHECK_FALSE(net.CheckSelection({winners[sample]}));
			const bool firstWins = (winners[sample] == 0) != reverse;
			CHECK(first.Activ() == firstWins);
			CHECK(second.Activ() == !firstWins);
			CHECK(input.GetToken() == 1);
		}
}

TEST_CASE("Meet conflicts select one producer even with spare capacity", "[conflict-characterization][legacy-policy]")
{
	CHSimulationCharacterization net;
	auto& output = net.Place(0, 10);
	auto& first = net.Transition();
	auto& second = net.Transition();
	net.Arc(first, output);
	net.Arc(second, output);
	net.Conflict(first);
	net.Conflict(second);
	net.Seed(1);
	CHECK_FALSE(net.CheckSelection({0}));
	CHECK(first.Activ());
	CHECK_FALSE(second.Activ());
	CHECK(output.GetToken() == 1);
}

TEST_CASE("Branch selection indexes all arcs including read-only arcs", "[conflict-characterization][legacy-defect]")
{
	for (auto kind : {CHConnector::TypeTest, CHConnector::TypeInhibitor})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(2);
		auto& read = net.Transition();
		auto& first = net.Transition();
		auto& second = net.Transition();
		net.Arc(input, read, kind == CHConnector::TypeTest ? 1 : 3, kind);
		net.Arc(input, first);
		net.Arc(input, second);
		net.Conflict(read);
		net.Conflict(first);
		net.Conflict(second);
		net.Seed(1); // Two active normal arcs counted, raw index zero retained.
		CHECK_FALSE(net.CheckSelection({0}));
		CHECK(read.Activ());
		CHECK_FALSE(first.Activ());
		CHECK_FALSE(second.Activ());
		CHECK(input.GetToken() == 2);
		CHECK(net.Time() == 1);
	}
}

TEST_CASE("Meet selection indexes all arcs including read-only arcs", "[conflict-characterization][legacy-defect]")
{
	for (auto kind : {CHConnector::TypeTest, CHConnector::TypeInhibitor})
	{
		CHSimulationCharacterization net;
		auto& output = net.Place(0, 10);
		auto& read = net.Transition();
		auto& first = net.Transition();
		auto& second = net.Transition();
		net.Arc(read, output, 1, kind);
		net.Arc(first, output);
		net.Arc(second, output);
		net.Conflict(read);
		net.Conflict(first);
		net.Conflict(second);
		net.Seed(1);
		CHECK_FALSE(net.CheckSelection({0}));
		CHECK(read.Activ());
		CHECK_FALSE(first.Activ());
		CHECK_FALSE(second.Activ());
		CHECK(output.GetToken() == 0);
		CHECK(net.Time() == 1);
	}
}

TEST_CASE("Parallel normal arcs can disable their own sole transition", "[conflict-characterization][legacy-defect]")
{
	for (unsigned seed : {1U, 10000U})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(2);
		auto& transition = net.Transition();
		net.Arc(input, transition);
		net.Arc(input, transition);
		net.Conflict(transition);
		net.Seed(seed);
		CHECK(net.CheckSelection({seed == 1 ? 0 : 1}));
		CHECK_FALSE(transition.Activ());
		CHECK(input.GetToken() == 2);
		CHECK(net.Logged(L"Deadlock"));
	}
}

TEST_CASE("A branch loser still processes its remaining input conflicts", "[conflict-characterization][legacy-policy]")
{
	CHSimulationCharacterization net;
	auto& left = net.Place(1);
	auto& right = net.Place(1);
	auto& current = net.Transition();
	auto& leftPeer = net.Transition();
	auto& rightFirst = net.Transition();
	auto& rightSecond = net.Transition();
	net.Arc(left, leftPeer);
	net.Arc(left, current);
	net.Arc(right, rightFirst);
	net.Arc(right, rightSecond);
	net.Arc(right, current);
	// Only register current: isolates continuation within its one outer iteration.
	net.Conflict(current);
	net.Seed(1); // Raw indices zero then one.
	CHECK_FALSE(net.CheckSelection({0, 1}));
	CHECK_FALSE(current.Activ());
	CHECK(leftPeer.Activ());
	CHECK_FALSE(rightFirst.Activ());
	CHECK(rightSecond.Activ());
	CHECK(left.GetToken() == 0);
	CHECK(right.GetToken() == 0);
}

TEST_CASE("A branch loser still processes its output meet conflicts", "[conflict-characterization][legacy-policy]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& output = net.Place(0, 10);
	auto& current = net.Transition();
	auto& branchPeer = net.Transition();
	auto& meetFirst = net.Transition();
	auto& meetSecond = net.Transition();
	net.Arc(input, branchPeer);
	net.Arc(input, current);
	net.Arc(meetFirst, output);
	net.Arc(meetSecond, output);
	net.Arc(current, output);
	net.Conflict(current);
	net.Seed(1);
	CHECK_FALSE(net.CheckSelection({0, 1}));
	CHECK_FALSE(current.Activ());
	CHECK(branchPeer.Activ());
	CHECK_FALSE(meetFirst.Activ());
	CHECK(meetSecond.Activ());
	CHECK(input.GetToken() == 0);
	CHECK(output.GetToken() == 1);
}

TEST_CASE("Reconnecting an arc changes conflict order without changing document order", "[conflict-characterization][legacy-policy]")
{
	using namespace hps::domain;
	for (bool branch : {true, false})
	{
		CHSimulationCharacterization net;
		auto& place = net.Place(branch ? 2 : 0, 10);
		auto& first = net.Transition();
		auto& second = net.Transition();
		auto& firstArc = branch ? net.Arc(place, first) : net.Arc(first, place);
		auto& secondArc = branch ? net.Arc(place, second) : net.Arc(second, place);
		if (branch) { firstArc.From(nullptr); firstArc.From(&place); }
		else { firstArc.To(nullptr); firstArc.To(&place); }
		auto& adjacency = branch ? place.ConnectorsOut() : place.ConnectorsIn();
		REQUIRE(adjacency.GetCount() == 2);
		CHECK(adjacency[0] == &secondArc);
		CHECK(adjacency[1] == &firstArc);
		const auto exported = hps::adapters::ExportLegacyNet(net.document);
		REQUIRE(std::holds_alternative<NetSnapshot>(exported));
		const auto& arcs = std::get<NetSnapshot>(exported).net.arcs;
		REQUIRE(arcs.size() == 2);
		CHECK(arcs[0].id.value == firstArc.Id());
		CHECK(arcs[1].id.value == secondArc.Id());
		net.Conflict(first);
		net.Conflict(second);
		net.Seed(1);
		CHECK_FALSE(net.CheckSelection({0}));
		CHECK_FALSE(first.Activ());
		CHECK(second.Activ());
		CHECK(place.GetToken() == 1);
	}
}

TEST_CASE("Pure selection matches chained legacy conflicts as a winner becomes blocked", "[selection-differential]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(3);
	auto& output = net.Place(0, 2);
	auto& first = net.Transition();
	auto& second = net.Transition();
	auto& third = net.Transition();
	net.Arc(input, first);
	net.Arc(input, second);
	net.Arc(input, third);
	net.Arc(first, output);
	net.Conflict(first);
	net.Conflict(second);
	net.Conflict(third);
	for (int step = 0; step < 3; ++step)
	{
		net.Seed(1);
		CHECK(net.CheckSelection({0}) == (step == 2));
	}
	CHECK(input.GetToken() == 1);
	CHECK(output.GetToken() == 2);
	CHECK_FALSE(first.Activ());
	CHECK_FALSE(second.Activ());
	CHECK_FALSE(third.Activ());
}

TEST_CASE("Pure selection replays separate branch and meet choices in order", "[selection-differential]")
{
	for (bool fast : {false, true})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(2);
		auto& output = net.Place(0, 10);
		auto& branchFirst = net.Transition();
		auto& branchSecond = net.Transition();
		auto& meetFirst = net.Transition();
		auto& meetSecond = net.Transition();
		net.Arc(input, branchFirst);
		net.Arc(input, branchSecond);
		net.Arc(meetFirst, output);
		net.Arc(meetSecond, output);
		for (auto* transition : {&branchFirst, &branchSecond, &meetFirst, &meetSecond}) net.Conflict(*transition);
		net.Seed(1);
		CHECK_FALSE(net.CheckSelection({0, 1}, fast));
		CHECK(branchFirst.Activ());
		CHECK_FALSE(branchSecond.Activ());
		CHECK_FALSE(meetFirst.Activ());
		CHECK(meetSecond.Activ());
		CHECK(input.GetToken() == 1);
		CHECK(output.GetToken() == 1);
	}
}

TEST_CASE("Potential conflict scan ignores arc kind eligibility and resource abundance", "[scan-differential]")
{
	using hps::domain::TransitionId;
	for (bool branch : {true, false})
		for (auto firstKind : {CHConnector::TypeNormal, CHConnector::TypeTest, CHConnector::TypeInhibitor})
			for (auto secondKind : {CHConnector::TypeNormal, CHConnector::TypeTest, CHConnector::TypeInhibitor})
				for (long tokens : {0L, 2L, 10L})
				{
					INFO("branch=" << branch << ", kinds=" << firstKind << "," << secondKind << ", tokens=" << tokens);
					CHSimulationCharacterization net;
					auto& place = net.Place(tokens, 10);
					auto& first = net.Transition(CHTransition::TTM_DELAY, 5);
					auto& second = net.Transition();
					if (branch) { net.Arc(place, first, 1, firstKind); net.Arc(place, second, 1, secondKind); }
					else { net.Arc(first, place, 1, firstKind); net.Arc(second, place, 1, secondKind); }
					for (auto* transition : {&first, &second})
					{
						transition->Enabled(false);
						transition->Invalid(false);
						transition->Activ(true);
					}
					net.CheckPreScan({TransitionId{first.Id()}, TransitionId{second.Id()}});
					for (auto* transition : {&first, &second})
					{
						CHECK(transition->Enabled());
						CHECK(transition->Invalid());
						CHECK(transition->Activ()); // Scan itself does not reset active.
					}
					CHECK(first.GetDelay() == 5);
					CHECK(place.GetToken() == tokens);
					CHECK(net.Steps() == 0);
				}
}

TEST_CASE("Potential scan lists a transition once in document order across branch and meet", "[scan-differential]")
{
	using hps::domain::TransitionId;
	CHSimulationCharacterization net;
	auto& input = net.Place(2);
	auto& output = net.Place(0);
	auto& isolated = net.Transition();
	auto& first = net.Transition();
	auto& second = net.Transition();
	auto& third = net.Transition();
	net.Arc(input, first);
	net.Arc(input, second);
	net.Arc(first, output);
	net.Arc(third, output);
	isolated.Enabled(false);
	isolated.Invalid(false);
	net.CheckPreScan({TransitionId{first.Id()}, TransitionId{second.Id()}, TransitionId{third.Id()}});
	CHECK_FALSE(isolated.Enabled());
	CHECK_FALSE(isolated.Invalid());
	// The caller clears the destination list before each scan, as Online does.
	net.CheckPreScan({TransitionId{first.Id()}, TransitionId{second.Id()}, TransitionId{third.Id()}});
}

TEST_CASE("Legacy scan skips disconnected arcs but counts them as place adjacency peers", "[scan-differential]")
{
	using namespace hps::domain;
	CHSimulationCharacterization net;
	auto& input = net.Place(2);
	auto& first = net.Transition();
	auto& second = net.Transition();
	net.Arc(input, first);
	auto& partial = net.Arc(input, second);
	SECTION("Only source remains") { partial.To(nullptr); }
	SECTION("Only destination remains") { partial.From(nullptr); }
	const auto exported = hps::adapters::ExportLegacySimulationSnapshot(net.document);
	REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(exported));
	CHECK(std::get<std::vector<ValidationIssue>>(exported).front().code == ValidationCode::DisconnectedArc);
	const auto actual = net.ScanLegacy(); // No unsafe SingleStep on malformed input.
	if (partial.From()) CHECK(actual == std::vector<TransitionId>{TransitionId{first.Id()}});
	else CHECK(actual.empty());
}

TEST_CASE("Actual pre-scan feeds pure selection after reconnection and with parallel arcs", "[scan-differential]")
{
	using hps::domain::TransitionId;
	SECTION("Reconnection")
	{
		for (bool branch : {true, false})
		{
			CHSimulationCharacterization net;
			auto& place = net.Place(branch ? 2 : 0);
			auto& first = net.Transition();
			auto& second = net.Transition();
			auto& arc = branch ? net.Arc(place, first) : net.Arc(first, place);
			if (branch) { net.Arc(place, second); arc.From(nullptr); arc.From(&place); }
			else { net.Arc(second, place); arc.To(nullptr); arc.To(&place); }
			net.CheckPreScan({TransitionId{first.Id()}, TransitionId{second.Id()}});
			net.Seed(1);
			CHECK_FALSE(net.CheckSelection({0}));
			CHECK_FALSE(first.Activ());
			CHECK(second.Activ());
		}
	}
	SECTION("Parallel arcs")
	{
		CHSimulationCharacterization net;
		auto& place = net.Place(2);
		auto& transition = net.Transition();
		net.Arc(place, transition);
		net.Arc(place, transition);
		net.CheckPreScan({TransitionId{transition.Id()}});
		net.Seed(1);
		CHECK(net.CheckSelection({0}));
	}
}

TEST_CASE("Complete pure steps replay a weighted chain through deadlock", "[step-differential]")
{
	for (bool fast : {false, true})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(4);
		auto& middle = net.Place(0);
		auto& output = net.Place(0);
		auto& first = net.Transition();
		auto& second = net.Transition();
		net.Arc(input, first, 2); net.Arc(first, middle);
		net.Arc(middle, second); net.Arc(second, output, 3);
		auto state = net.BeginStepComparison();
		auto clock = net.Clock();
		for (int index = 0; index < 4; ++index)
		{
			auto plan = net.CheckFullStep(state.snapshot, state.topology, clock, {}, fast);
			CHECK(plan.stop.deadlock == (index == 3));
			state.snapshot = std::move(plan.snapshotAfter);
			clock = plan.clockAfter;
		}
		CHECK(output.GetToken() == 6);
		CHECK(first.GetTokensCount() == 4);
		CHECK(second.GetTokensCount() == 2);
	}
}

TEST_CASE("Complete pure steps replay competing timers and alternate conflict winners", "[step-differential]")
{
	CHSimulationCharacterization net;
	auto& input = net.Place(2);
	auto& firstOutput = net.Place(0);
	auto& secondOutput = net.Place(0);
	auto& first = net.Transition(CHTransition::TTM_DELAY, 2);
	auto& second = net.Transition(CHTransition::TTM_DELAY, 2);
	net.Arc(input, first); net.Arc(input, second);
	net.Arc(first, firstOutput); net.Arc(second, secondOutput);
	auto state = net.BeginStepComparison();
	auto clock = net.Clock();
	for (int index = 0; index < 7; ++index)
	{
		hps::domain::StepRandomInputs random;
		if (index == 2) { net.Seed(1); random.conflictChoices = {0}; }
		if (index == 5) { net.Seed(10000); random.conflictChoices = {1}; }
		auto plan = net.CheckFullStep(state.snapshot, state.topology, clock, random);
		CHECK(plan.stop.deadlock == (index == 6));
		state.snapshot = std::move(plan.snapshotAfter);
		clock = plan.clockAfter;
	}
	CHECK(firstOutput.GetToken() == 1);
	CHECK(secondOutput.GetToken() == 1);
	CHECK(clock.timeMs == 4);
}

TEST_CASE("Complete pure steps replay stochastic delays without reading legacy samples", "[step-differential]")
{
	using namespace hps::domain;
	for (auto mode : {CHTransition::TTM_EQUAL_DISTR, CHTransition::TTM_EXPONENTIAL})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(2);
		auto& output = net.Place(0);
		auto& transition = net.Transition(mode, mode == CHTransition::TTM_EQUAL_DISTR ? 2 : 10, 7);
		transition.SetDelay(0);
		net.Arc(input, transition); net.Arc(transition, output);
		auto state = net.BeginStepComparison();
		auto clock = net.Clock();
		net.Seed(1);
		const int firstDelay = mode == CHTransition::TTM_EQUAL_DISTR ? 2 : 5;
		const int secondDelay = mode == CHTransition::TTM_EQUAL_DISTR ? 4 : 10;
		for (int index = 0; index <= firstDelay + 2; ++index)
		{
			StepRandomInputs random;
			if (index == 0) random.delays.emplace(TransitionId{transition.Id()}, firstDelay);
			if (index == firstDelay + 1) random.delays.emplace(TransitionId{transition.Id()}, secondDelay);
			auto plan = net.CheckFullStep(state.snapshot, state.topology, clock, random);
			CHECK(plan.stop.deadlock == (index == firstDelay + 2));
			state.snapshot = std::move(plan.snapshotAfter);
			clock = plan.clockAfter;
		}
		CHECK(output.GetToken() == 2);
		CHECK(clock.timeMs == firstDelay);
		CHECK(state.snapshot.transitionStates.at(TransitionId{transition.Id()}).remainingDelayMs == secondDelay);
	}
}

TEST_CASE("Complete pure step retains mixed-arc policy and simultaneous limits", "[step-differential]")
{
	for (bool fast : {false, true})
		for (auto kind : {CHConnector::TypeTest, CHConnector::TypeInhibitor})
		{
			CHSimulationCharacterization net;
			auto& input = net.Place(2);
			auto& read = net.Transition();
			auto& first = net.Transition();
			auto& second = net.Transition();
			net.Arc(input, read, kind == CHConnector::TypeTest ? 1 : 3, kind);
			net.Arc(input, first); net.Arc(input, second);
			net.Limits(0, 0);
			const auto state = net.BeginStepComparison();
			net.Seed(1);
			const auto plan = net.CheckFullStep(state.snapshot, state.topology, net.Clock(), {{0}, {}}, fast);
			CHECK_FALSE(plan.stop.deadlock);
			CHECK(plan.stop.timeLimitReached);
			CHECK(plan.stop.stepLimitReached);
			CHECK(input.GetToken() == 2);
		}
}

TEST_CASE("Engine step and legacy reset agree for every transition time mode", "[engine-differential]")
{
	using namespace hps::domain;
	using hps::simulation::SimulationEngine;
	for (auto mode : {CHTransition::TTM_IMMIDIATE, CHTransition::TTM_DELAY,
		CHTransition::TTM_EQUAL_DISTR, CHTransition::TTM_EXPONENTIAL})
	{
		CHSimulationCharacterization net;
		auto& input = net.Place(2);
		CHPropertyValues properties;
		properties.Add(CHPropertyValue(5L, ID_POSITION_TOKEN_INITIAL));
		REQUIRE(input.SetProperties(properties, false));
		input.SetToken(2); input.SetTokenCount(9);
		auto& transition = net.Transition(mode, 5, 10);
		transition.SetDelay(2); transition.SetTokensCount(7);
		net.Arc(input, transition);
		const auto prepared = net.BeginStepComparison();
		auto created = SimulationEngine::Create(prepared.snapshot, prepared.topology, net.Clock());
		REQUIRE(std::holds_alternative<SimulationEngine>(created));
		auto engine = std::get<SimulationEngine>(std::move(created));
		const auto before = engine.Snapshot();
		REQUIRE(std::holds_alternative<SimulationStepPlan>(engine.Step()));
		net.CheckFullStep(before.net, prepared.topology, before.clock);
		net.CheckEngineState(engine.Snapshot());
		engine.ResetLegacy(); net.Reset();
		net.CheckEngineState(engine.Snapshot(), true);
		CHECK(input.GetToken() == 5);
		CHECK(transition.GetDelay() == (mode == CHTransition::TTM_IMMIDIATE ? 5 : 1));
	}
}

TEST_CASE("Engine owns repeated chain steps and resumes after legacy reset", "[engine-differential]")
{
	using namespace hps::domain;
	using hps::simulation::SimulationEngine;
	CHSimulationCharacterization net;
	auto& input = net.Place(4);
	CHPropertyValues properties;
	properties.Add(CHPropertyValue(4L, ID_POSITION_TOKEN_INITIAL));
	REQUIRE(input.SetProperties(properties, false));
	auto& middle = net.Place(0);
	auto& output = net.Place(0);
	auto& first = net.Transition(); auto& second = net.Transition();
	net.Arc(input, first, 2); net.Arc(first, middle);
	net.Arc(middle, second); net.Arc(second, output, 3);
	const auto prepared = net.BeginStepComparison();
	auto created = SimulationEngine::Create(prepared.snapshot, prepared.topology, net.Clock());
	REQUIRE(std::holds_alternative<SimulationEngine>(created));
	auto engine = std::get<SimulationEngine>(std::move(created));
	for (int round = 0; round < 2; ++round)
	{
		for (int index = 0; index < 4; ++index)
		{
			const auto before = engine.Snapshot();
			REQUIRE(std::holds_alternative<SimulationStepPlan>(engine.Step()));
			net.CheckFullStep(before.net, prepared.topology, before.clock);
			net.CheckEngineState(engine.Snapshot());
		}
		CHECK(output.GetToken() == 6);
		engine.ResetLegacy(); net.Reset();
		net.CheckEngineState(engine.Snapshot(), true);
	}
}

TEST_CASE("Engine sample error leaves state untouched before successful legacy replay", "[engine-differential]")
{
	using namespace hps::domain;
	using hps::simulation::SimulationEngine;
	CHSimulationCharacterization net;
	auto& input = net.Place(1);
	auto& output = net.Place(0);
	auto& transition = net.Transition(CHTransition::TTM_EQUAL_DISTR, 2, 7);
	transition.SetDelay(0);
	net.Arc(input, transition); net.Arc(transition, output);
	const auto prepared = net.BeginStepComparison();
	auto created = SimulationEngine::Create(prepared.snapshot, prepared.topology, net.Clock());
	REQUIRE(std::holds_alternative<SimulationEngine>(created));
	auto engine = std::get<SimulationEngine>(std::move(created));
	const auto before = engine.Snapshot();
	REQUIRE(std::holds_alternative<std::vector<ValidationIssue>>(engine.Step()));
	net.CheckEngineState(engine.Snapshot());
	net.Seed(1);
	const StepRandomInputs random{{}, {{TransitionId{transition.Id()}, 2}}};
	REQUIRE(std::holds_alternative<SimulationStepPlan>(engine.Step(random)));
	net.CheckFullStep(before.net, prepared.topology, before.clock, random);
	net.CheckEngineState(engine.Snapshot());
	engine.ResetLegacy(); net.Reset();
	net.CheckEngineState(engine.Snapshot(), true);
	CHECK(transition.GetDelay() == 2);
}
