#pragma once

// Test automata for the schema jump-forward host suite. Each is a hand-written token DFA: explicit
// transitions and an explicit forced set, written here from the cell it serves and never from the
// code under test. The forced set plays the OBSERVED flags (rev 1.3.5): what the subsystem read
// through sslm_seq_forced_tokens, which for these automata is the schema's own forcedness.
//
// The B7 fixtures (fixtures/): d2-table.txt, B7's shipped table, verbatim (sha256 4cc216d2...), and
// b4a-b2-walked.txt, B4a's 20 off streams and B2's off stream walked through potion_shop_order, with
// the bench's learned drafts on B2's prompt. LoadB7 reads both, by code written here.

#include "SuperSLMSchemaJumpCore.h"

#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <set>
#include <utility>
#include <vector>

namespace SchemaJumpFixture
{
	using SuperSLMCore::ISchemaAutomaton;

	class FFakeAutomaton final : public ISchemaAutomaton
	{
	public:
		uint32_t Start = 0;
		std::map<std::pair<uint32_t, int32_t>, uint32_t> Edges;
		std::set<uint32_t> Forced;

		void Edge(uint32_t From, int32_t Token, uint32_t To) { Edges[{From, Token}] = To; }

		uint32_t StartState() const override { return Start; }

		bool Step(uint32_t State, int32_t Token, uint32_t& OutNext) const override
		{
			const auto It = Edges.find({State, Token});
			if (It == Edges.end())
			{
				return false;
			}
			OutNext = It->second;
			return true;
		}

		bool IsForced(uint32_t State) const override { return Forced.count(State) != 0; }
	};

	// A potion-order-shaped schema in miniature (plan §5.2's example, reduced):
	//   S0 forced    100 -> S1, 101 -> S5     (two splits of the same fixed text)
	//   S1 forced    102 -> S2
	//   S5 forced    105 -> S2
	//   S2 forced    103 -> S3
	//   S3 choice    200 -> S4, 201 -> S4, 202 -> S6
	//   S4 forced    104 -> S7
	//   S6 choice    203 -> S7
	//   S7 accepting (no row, unforced)
	enum : uint32_t
	{
		S0 = 0, S1 = 1, S2 = 2, S3 = 3, S4 = 4, S5 = 5, S6 = 6, S7 = 7,
	};
	constexpr size_t kOrderForcedStates = 5; // S0, S1, S2, S4, S5

	inline FFakeAutomaton OrderAutomaton()
	{
		FFakeAutomaton A;
		A.Start = S0;
		A.Edge(S0, 100, S1);
		A.Edge(S0, 101, S5);
		A.Edge(S1, 102, S2);
		A.Edge(S5, 105, S2);
		A.Edge(S2, 103, S3);
		A.Edge(S3, 200, S4);
		A.Edge(S3, 201, S4);
		A.Edge(S3, 202, S6);
		A.Edge(S4, 104, S7);
		A.Edge(S6, 203, S7);
		A.Forced = {S0, S1, S2, S4, S5};
		return A;
	}

	// The two complete streams the order schema admits along its S1 and S5 splits.
	inline std::vector<int32_t> OrderStreamA() { return {100, 102, 103, 200, 104}; }
	inline std::vector<int32_t> OrderStreamB() { return {101, 105, 103, 201, 104}; }

	// A forced chain of Length states, G0 .. G(Length-1), each forced, each admitting tokens
	// 1000 + 10 * i + k for k in 0..3 (all to the next state); G(Length) is an unforced end state.
	// Token(i, k) names them.
	inline int32_t ChainToken(uint32_t I, int32_t K) { return 1000 + 10 * static_cast<int32_t>(I) + K; }

	inline FFakeAutomaton ChainAutomaton(uint32_t Length)
	{
		FFakeAutomaton A;
		A.Start = 0;
		for (uint32_t I = 0; I < Length; ++I)
		{
			for (int32_t K = 0; K < 4; ++K)
			{
				A.Edge(I, ChainToken(I, K), I + 1);
			}
			A.Forced.insert(I);
		}
		return A;
	}

	// The perfect stream through ChainAutomaton: token k = 0 at every state.
	inline std::vector<int32_t> ChainStream(uint32_t Length)
	{
		std::vector<int32_t> Out;
		for (uint32_t I = 0; I < Length; ++I)
		{
			Out.push_back(ChainToken(I, 0));
		}
		return Out;
	}

	// Seed rows for ChainAutomaton: Counts[i] = {count of k=0, count of k=1, ...} at state i.
	inline std::vector<SuperSLMCore::FDraftSeedRow> ChainSeed(const std::vector<std::vector<uint32_t>>& Counts)
	{
		std::vector<SuperSLMCore::FDraftSeedRow> Rows;
		for (uint32_t I = 0; I < Counts.size(); ++I)
		{
			for (size_t K = 0; K < Counts[I].size(); ++K)
			{
				if (Counts[I][K] > 0)
				{
					Rows.push_back({I, ChainToken(I, static_cast<int32_t>(K)), Counts[I][K]});
				}
			}
		}
		return Rows;
	}

	// Limits wide enough that only the table decides the draft.
	inline SuperSLMCore::FDraftLimits Wide()
	{
		SuperSLMCore::FDraftLimits L;
		L.MaxPrefillChunkBudget = 64;
		L.RemainingAllowance = 64;
		L.bThreadedDecodeEffective = false;
		return L;
	}

	// --- The B7 fixtures (P1X.12 (a)) ---

#ifndef SCHEMA_JUMP_FIXTURE_DIR
#define SCHEMA_JUMP_FIXTURE_DIR "fixtures"
#endif

	struct FB7Draft
	{
		size_t Position = 0;   // the stream position the draft starts at (after the pending token)
		uint32_t State = 0;    // the walk state there
		int32_t Cap = 0;       // the bench's cap: room - 1
		std::vector<int32_t> Draft;
		size_t Confirmed = 0;  // the tokens the verb confirmed (off's next tokens)
	};

	struct FB7
	{
		bool bLoaded = false;
		std::string D2Text;                                // d2-table.txt's bytes
		uint32_t StateCount = 0;                           // its "states" line
		std::set<uint32_t> Forced;                         // its "forced" line: the library's flags
		std::map<uint32_t, std::map<int32_t, uint64_t>> Rows; // its "S" rows
		std::vector<std::vector<int32_t>> B4aStreams;
		std::vector<int32_t> B2Stream;
		std::vector<FB7Draft> B2Drafts;
		FFakeAutomaton Walked;                             // the walked edges; Forced = the library's flags
	};

	namespace Detail
	{
		inline bool ReadFile(const std::string& Path, std::string& Out)
		{
			std::ifstream In(Path, std::ios::binary);
			if (!In)
			{
				return false;
			}
			std::ostringstream Ss;
			Ss << In.rdbuf();
			Out = Ss.str();
			return true;
		}

		// "W s:t s:t ..." pairs, then the "E end" line; adds every edge to A and returns the tokens.
		inline std::vector<int32_t> AddWalk(FFakeAutomaton& A, std::istringstream& Pairs, uint32_t End)
		{
			std::vector<std::pair<uint32_t, int32_t>> Steps;
			std::string P;
			while (Pairs >> P)
			{
				const size_t C = P.find(':');
				Steps.push_back({static_cast<uint32_t>(std::stoul(P.substr(0, C))), static_cast<int32_t>(std::stol(P.substr(C + 1)))});
			}
			std::vector<int32_t> Tokens;
			for (size_t I = 0; I < Steps.size(); ++I)
			{
				A.Edge(Steps[I].first, Steps[I].second, I + 1 < Steps.size() ? Steps[I + 1].first : End);
				Tokens.push_back(Steps[I].second);
			}
			return Tokens;
		}
	}

	inline const FB7& LoadB7()
	{
		static FB7 B;
		static bool bTried = false;
		if (bTried)
		{
			return B;
		}
		bTried = true;
		const std::string Dir = SCHEMA_JUMP_FIXTURE_DIR;
		std::string Walked;
		if (!Detail::ReadFile(Dir + "/d2-table.txt", B.D2Text) || !Detail::ReadFile(Dir + "/b4a-b2-walked.txt", Walked))
		{
			return B;
		}
		{
			std::istringstream In(B.D2Text);
			std::string Line;
			while (std::getline(In, Line))
			{
				std::istringstream Ls(Line);
				std::string Tag;
				Ls >> Tag;
				if (Tag == "states")
				{
					Ls >> B.StateCount;
				}
				else if (Tag == "forced")
				{
					uint32_t S = 0;
					while (Ls >> S)
					{
						B.Forced.insert(S);
					}
				}
				else if (Tag == "S")
				{
					uint32_t S = 0;
					Ls >> S;
					std::string Tc;
					while (Ls >> Tc)
					{
						const size_t C = Tc.find(':');
						B.Rows[S][static_cast<int32_t>(std::stol(Tc.substr(0, C)))] += std::stoull(Tc.substr(C + 1));
					}
				}
			}
		}
		{
			std::istringstream In(Walked);
			std::string Line;
			std::string Pending;
			std::string PendingTag;
			while (std::getline(In, Line))
			{
				if (!Line.empty() && Line.back() == '\r')
				{
					Line.pop_back();
				}
				std::istringstream Ls(Line);
				std::string Tag;
				Ls >> Tag;
				if (Tag == "B4A" || Tag == "B2")
				{
					PendingTag = Tag;
					Pending = Line.substr(Tag.size());
				}
				else if (Tag == "E")
				{
					uint32_t End = 0;
					Ls >> End;
					std::istringstream Pairs(Pending);
					const std::vector<int32_t> Tokens = Detail::AddWalk(B.Walked, Pairs, End);
					if (PendingTag == "B4A")
					{
						B.B4aStreams.push_back(Tokens);
					}
					else
					{
						B.B2Stream = Tokens;
					}
				}
				else if (Tag == "D")
				{
					FB7Draft D;
					Ls >> D.Position >> D.State >> D.Cap;
					std::string T;
					while (Ls >> T && T != "|")
					{
						D.Draft.push_back(static_cast<int32_t>(std::stol(T)));
					}
					Ls >> D.Confirmed;
					B.B2Drafts.push_back(D);
				}
			}
		}
		B.Walked.Start = 0;
		B.Walked.Forced = B.Forced;
		B.bLoaded = B.StateCount > 0 && !B.Forced.empty() && B.B4aStreams.size() == 20 && !B.B2Stream.empty() && !B.B2Drafts.empty();
		return B;
	}
}
