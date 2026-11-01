#include "grid.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#ifndef GRID_PATH
#error "GRID_PATH must name grid.txt"
#endif

namespace MultiRowHost
{
	namespace
	{
		std::map<std::string, std::string> Fields(const std::string& Line)
		{
			std::map<std::string, std::string> Out;
			std::istringstream In(Line);
			std::string Tok;
			In >> Tok;  // "in" / "out"
			while (In >> Tok)
			{
				const size_t Eq = Tok.find('=');
				if (Eq != std::string::npos) Out[Tok.substr(0, Eq)] = Tok.substr(Eq + 1);
			}
			return Out;
		}
		template <typename T>
		bool List(const std::string& Text, T* Out, int N)
		{
			int I = 0;
			const char* P = Text.c_str();
			while (*P && I < N)
			{
				char* End = nullptr;
				const long long V = std::strtoll(P, &End, 10);
				if (End == P) return false;
				Out[I++] = static_cast<T>(V);
				P = *End == ',' ? End + 1 : End;
			}
			return I == N;
		}
		long long Int(const std::map<std::string, std::string>& F, const char* K, bool* Ok)
		{
			const auto It = F.find(K);
			if (It == F.end()) { *Ok = false; return 0; }
			return std::strtoll(It->second.c_str(), nullptr, 10);
		}
		double Dbl(const std::map<std::string, std::string>& F, const char* K, bool* Ok)
		{
			const auto It = F.find(K);
			if (It == F.end()) { *Ok = false; return 0.0; }
			return std::strtod(It->second.c_str(), nullptr);  // repr() of a double round-trips exactly
		}
	}

	std::vector<FCase> LoadGrid(std::string* Error)
	{
		std::vector<FCase> Out;
		std::FILE* F = std::fopen(GRID_PATH, "r");
		if (!F) { *Error = std::string("cannot open ") + GRID_PATH; return {}; }
		char Buf[4096];
		FCase Cur;
		bool HaveIn = false;
		int LineNo = 0;
		while (std::fgets(Buf, sizeof Buf, F))
		{
			++LineNo;
			const std::string Line(Buf);
			if (Line.rfind("in ", 0) == 0)
			{
				const auto Fi = Fields(Line);
				bool Ok = true;
				Cur = FCase{};
				Cur.Line = LineNo;
				Cur.Table = Fi.count("table") ? Fi.at("table") : "";
				auto& I = Cur.In;
				Ok = Ok && Fi.count("r") && List(Fi.at("r"), I.CostRatioPerMille, 8) && Fi.count("g") && List(Fi.at("g"), I.LogitsRatioPerMille, 8);
				I.bTableMeasured = Int(Fi, "meas", &Ok) != 0;
				I.GpuRowCostMs = Dbl(Fi, "c1", &Ok);
				I.FrameBudgetMs = Dbl(Fi, "b", &Ok);
				I.SceneGpuShareMs = Dbl(Fi, "s", &Ok);
				I.NominalTickMs = Dbl(Fi, "nt", &Ok);
				I.GpuRowsMarginPerMille = static_cast<int32_t>(Int(Fi, "m", &Ok));
				I.bGpuMultiRow = Int(Fi, "mr", &Ok) != 0;
				I.Layers = static_cast<uint32_t>(Int(Fi, "L", &Ok));
				I.LayersPerTick = static_cast<uint32_t>(Int(Fi, "lpt", &Ok));
				I.DispatchesPerLayer = static_cast<uint32_t>(Int(Fi, "dpl", &Ok));
				I.Path = Int(Fi, "path", &Ok) ? SuperSLMCore::GpuRows::EPath::Composed : SuperSLMCore::GpuRows::EPath::OneCall;
				I.bExactChosen = Int(Fi, "ex", &Ok) != 0;
				I.bDeviceLogitsHead = Int(Fi, "head", &Ok) != 0;
				I.bShippingBuild = Int(Fi, "ship", &Ok) != 0;
				if (!Ok) { *Error = "bad in line " + std::to_string(LineNo); std::fclose(F); return {}; }
				HaveIn = true;
			}
			else if (Line.rfind("out ", 0) == 0 && HaveIn)
			{
				const auto Fo = Fields(Line);
				bool Ok = true;
				auto& W = Cur.Want;
				W.Status = static_cast<int32_t>(Int(Fo, "status", &Ok));
				W.MultiRowOn = static_cast<int32_t>(Int(Fo, "mron", &Ok));
				W.BelowOneRow = static_cast<int32_t>(Int(Fo, "below", &Ok));
				W.RowCostUs = Int(Fo, "c1us", &Ok);
				W.SceneUs = Int(Fo, "sus", &Ok);
				W.BudgetUs = Int(Fo, "bus", &Ok);
				Ok = Ok && Fo.count("C") && List(Fo.at("C"), W.CostUs, 8) && Fo.count("G") && List(Fo.at("G"), W.LogitsUs, 8) && Fo.count("l") &&
				     List(Fo.at("l"), W.LayersPerSlice, 8) && Fo.count("D") && List(Fo.at("D"), W.Ticks, 8) && Fo.count("Dx") &&
				     List(Fo.at("Dx"), W.ExactTicks, 8);
				W.MMax = static_cast<int32_t>(Int(Fo, "mmax", &Ok));
				W.MMaxLayers = static_cast<int32_t>(Int(Fo, "ml", &Ok));
				W.ComposedRows = static_cast<int32_t>(Int(Fo, "cr", &Ok));
				W.ComposedLayers = static_cast<int32_t>(Int(Fo, "cl", &Ok));
				W.ComposedUnits = static_cast<int32_t>(Int(Fo, "cu", &Ok));
				W.ExactOn = static_cast<int32_t>(Int(Fo, "exon", &Ok));
				W.ExactReason = static_cast<int32_t>(Int(Fo, "reason", &Ok));
				W.MExact = static_cast<int32_t>(Int(Fo, "mexact", &Ok));
				if (!Ok) { *Error = "bad out line " + std::to_string(LineNo); std::fclose(F); return {}; }
				Out.push_back(Cur);
				HaveIn = false;
			}
		}
		std::fclose(F);
		if (Out.empty()) *Error = "no cases in grid";
		return Out;
	}

	bool PlanMatches(const SuperSLMCore::GpuRows::FChooserPlan& P, const FExpected& W, std::vector<std::string>* Diffs)
	{
		const size_t Before = Diffs->size();
		auto Eq = [&](const char* Name, long long Have, long long Want) {
			if (Have != Want) Diffs->push_back(std::string(Name) + " " + std::to_string(Have) + ", want " + std::to_string(Want));
		};
		Eq("bValid", P.bValid ? 1 : 0, 1);
		Eq("table status", static_cast<long long>(P.TableStatus), W.Status);
		Eq("multi-row on", P.bMultiRowOn ? 1 : 0, W.MultiRowOn);
		Eq("below one row", P.bBudgetBelowOneRow ? 1 : 0, W.BelowOneRow);
		Eq("C(1) us", P.RowCostUs, W.RowCostUs);
		Eq("scene us", P.SceneUs, W.SceneUs);
		Eq("B_eff us", P.BudgetUs, W.BudgetUs);
		Eq("l(M=1)", P.LayersPerSlice[0], W.LayersPerSlice[0]);
		Eq("D(M=1)", P.Ticks[0], W.Ticks[0]);
		if (W.MultiRowOn)
			for (int M = 1; M <= 8; ++M)
			{
				const std::string Tag = "(M=" + std::to_string(M) + ")";
				Eq(("C" + Tag).c_str(), P.CostUs[M - 1], W.CostUs[M - 1]);
				Eq(("G" + Tag).c_str(), P.LogitsUs[M - 1], W.LogitsUs[M - 1]);
				if (M == 1) continue;
				Eq(("l" + Tag).c_str(), P.LayersPerSlice[M - 1], W.LayersPerSlice[M - 1]);
				Eq(("D" + Tag).c_str(), P.Ticks[M - 1], W.Ticks[M - 1]);
				Eq(("D_x" + Tag).c_str(), P.ExactTicks[M - 1], W.ExactTicks[M - 1]);
			}
		Eq("M_max", P.MMax, W.MMax);
		Eq("l(M_max)", P.MMaxLayers, W.MMaxLayers);
		Eq("Composed rows", P.ComposedRows, W.ComposedRows);
		Eq("Composed layers", P.ComposedLayers, W.ComposedLayers);
		Eq("Composed units", P.ComposedUnits, W.ComposedUnits);
		Eq("Exact on", P.bExactOn ? 1 : 0, W.ExactOn);
		Eq("M_exact", P.MExact, W.MExact);
		if (W.ExactReason >= 0) Eq("Exact reason", static_cast<long long>(P.ExactReason), W.ExactReason);
		return Diffs->size() == Before;
	}
}
