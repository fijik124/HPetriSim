#include "stdafx.h"
#include "PetriSimDoc.h"
#include "resource.h"
#include "adapters/LegacyNetAdapter.h"
#include "domain/SimulationRules.h"
#include "domain/TokenEffects.h"
#include "domain/Timing.h"
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
	hps::adapters::ExportResult ExportWithOnlineFlag()
	{
		// Exercise the adapter's guard without starting an unsafe legacy worker.
		struct RestoreFlag
		{
			bool& flag;
			bool saved;
			~RestoreFlag() { flag = saved; }
		} restore{document.Sim().m_Online, document.Sim().m_Online};
		document.Sim().m_Online = true;
		return hps::adapters::ExportLegacyNet(document);
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
	const auto result = net.ExportWithOnlineFlag();
	using Issues = std::vector<hps::domain::ValidationIssue>;
	REQUIRE(std::holds_alternative<Issues>(result));
	const auto& issues = std::get<Issues>(result);
	REQUIRE(issues.size() == 1);
	CHECK(issues[0].code == hps::domain::ValidationCode::SnapshotUnavailable);
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
