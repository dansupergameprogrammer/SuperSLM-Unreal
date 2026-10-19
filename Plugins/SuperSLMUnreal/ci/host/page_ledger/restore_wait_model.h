#pragma once

// A C++ port of the plugin paging plan's restore wait model (the restore wait probe that sits
// beside the plan), so the C++ ledger is checked against the model's
// admissions on the same scenarios.
//
// The port is line-for-line in its rule:
//   - restores arrive in order; each is admitted when its charge fits and nobody waits;
//   - otherwise it waits only if
//         free + sum(guaranteed(r) for r ahead, in flight or waiting) - sum(charge(w) for w waiting) >= c
//     and is refused at arrival otherwise;
//   - applies run FIFO; an apply returns charge - settled_hold, where settled_hold is 0 for a
//     sharing restore and c - G for a private one; waiters are admitted at the apply that makes
//     them fit, in order; a waiter left when nothing is in flight is "short".
//   - the mutant counts a hinted restore's whole charge as guaranteed.
//
// Two generalizations, both off for the probe's own constructions (where they change nothing):
//   - guaranteed(r) = min(c, G) rather than G (a restore whose charge is below G cannot return
//     G to H; equal to the probe whenever c >= G);
//   - settled_hold = max(0, draw - G), with draw = R(b) for a sharing restore and c for a private
//     one (the probe's 0 / c - G when b == SequenceBudget).

#include <cstdint>
#include <vector>

namespace ledgertest
{

enum class EModelShare : int
{
	Unhinted = 0,   // the probe's None
	HintFails = 1,  // the probe's False
	HintShares = 2  // the probe's True
};

struct FModelRestore
{
	uint32_t Charge = 0;      // c
	uint32_t SharedDraw = 0;  // R(b): the draw when the hint shares
	EModelShare Share = EModelShare::Unhinted;
};

enum class EModelOutcome : int
{
	Admitted = 0,
	Waiting = 1,
	Refused = 2
};

struct FModelResult
{
	std::vector<EModelOutcome> Outcomes;
	uint32_t ShortWaiters = 0;
	uint32_t WaitersAdmitted = 0;
};

inline FModelResult RunRestoreWaitModel(const std::vector<FModelRestore>& Reqs, uint32_t H, uint32_t G, bool bMutant)
{
	FModelResult Out;
	int64_t Free = H;
	std::vector<size_t> InFlight;
	std::vector<size_t> Waiting;
	auto Guaranteed = [&](size_t I) -> int64_t {
		const FModelRestore& R = Reqs[I];
		if (bMutant && R.Share != EModelShare::Unhinted) return R.Charge;
		return R.Charge < G ? R.Charge : G;
	};
	for (size_t I = 0; I < Reqs.size(); ++I)
	{
		const int64_t C = Reqs[I].Charge;
		if (Waiting.empty() && Free >= C)
		{
			Free -= C;
			InFlight.push_back(I);
			Out.Outcomes.push_back(EModelOutcome::Admitted);
			continue;
		}
		int64_t Promise = Free;
		for (size_t J : InFlight) Promise += Guaranteed(J);
		for (size_t J : Waiting) Promise += Guaranteed(J) - static_cast<int64_t>(Reqs[J].Charge);
		if (Promise >= C)
		{
			Waiting.push_back(I);
			Out.Outcomes.push_back(EModelOutcome::Waiting);
		}
		else
		{
			Out.Outcomes.push_back(EModelOutcome::Refused);
		}
	}
	while (!InFlight.empty())
	{
		const size_t I = InFlight.front();
		InFlight.erase(InFlight.begin());
		const FModelRestore& R = Reqs[I];
		const int64_t Draw = R.Share == EModelShare::HintShares ? R.SharedDraw : R.Charge;
		const int64_t Hold = Draw > G ? Draw - G : 0;
		Free += static_cast<int64_t>(R.Charge) - Hold;
		while (!Waiting.empty() && Free >= Reqs[Waiting.front()].Charge)
		{
			const size_t W = Waiting.front();
			Waiting.erase(Waiting.begin());
			Free -= Reqs[W].Charge;
			InFlight.push_back(W);
			++Out.WaitersAdmitted;
		}
		if (InFlight.empty() && !Waiting.empty())
		{
			Out.ShortWaiters += static_cast<uint32_t>(Waiting.size());
			Waiting.clear();
		}
	}
	return Out;
}

}  // namespace ledgertest
