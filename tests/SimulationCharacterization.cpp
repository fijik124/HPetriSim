#include "stdafx.h"
#include "PetriSimDoc.h"
#include "resource.h"
#include "adapters/LegacyNetAdapter.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <memory>
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
		return result;
	}
	bool Step(bool fast = true)
	{
		bool locked = false;
		REQUIRE(document.Sim().SingleStep(fast, locked));
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
