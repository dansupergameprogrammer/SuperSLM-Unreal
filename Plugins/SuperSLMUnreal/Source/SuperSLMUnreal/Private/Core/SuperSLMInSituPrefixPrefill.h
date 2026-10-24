#pragma once

// The in-situ prefix_prefill arm (precache plan rev 1.18.12, §15.32; cell 5.9 (d)). Plain C++, no engine and no
// Unreal types, so the host suite tests it (ci/host/prefix_cache, test_insitu_prefix_prefill.cpp).
//
// §15.32: the isolated arm of SuperSLM.EstimateCosts times sslm_prefix_prefill with the game thread blocked, outside
// the worker lanes and away from the build's interleaved generation, and the box read its pair 1.5x low against the
// chunks 5.9 (d) grades. The pair is therefore fitted from the ledger's own chunk rows of a real cache build run
// through the subsystem (begin and chunk jobs on a worker lane, the resolved decode mode, task count and threads, the
// game ticking, and 5.9 (c)'s one generation on the build's lane): the same rows (d) grades, with no margin factor.
// This header holds the three pure parts of that arm:
//   - a ledger chunk row as a reading (its start depth recovered from its position sum);
//   - the fit of a build's readings, in depth order, with the isolated arm's fit (FitPrefixPrefillPair);
//   - the rewrite of the two keys in one section of CalibratedCosts.ini's text, every other line kept.
// USuperSLMSubsystem::WriteInSituPrefixPrefillPrice composes them with the ledger and the file.

#include "SuperSLMCacheChunkPricing.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace SuperSLMCore
{
	// A ledger chunk row (a cache build's DecodeOrPrefill job of PromptTokens tokens, PromptPositionSum the sum of
	// their positions, WorkerCallMs its measured worker time) as a reading. False for a row the pair does not price
	// (under kCacheChunkTiledMinTokens tokens: the chunk-1 pair prices it), a worker time that is not a finite,
	// positive number of milliseconds (-1 until measured), or a position sum that is not C consecutive positions
	// from a start depth of 0 or more.
	inline bool ChunkRowReading(int32_t PromptTokens, int64_t PromptPositionSum, double WorkerCallMs, FPrefixPrefillReading& Out)
	{
		if (PromptTokens < kCacheChunkTiledMinTokens || !std::isfinite(WorkerCallMs) || !(WorkerCallMs > 0.0))
		{
			return false;
		}
		const int64_t C = PromptTokens;
		const int64_t Above = PromptPositionSum - C * (C - 1) / 2;
		if (Above < 0 || Above % C != 0)
		{
			return false;
		}
		Out = FPrefixPrefillReading{ Above / C, PromptTokens, WorkerCallMs };
		return true;
	}

	// The in-situ pair: the readings sorted by start depth, then the isolated arm's fit (kPrefixPrefillFitGroups
	// groups, the upper envelope of their medians). False where that fit is.
	inline bool FitInSituPrefixPrefillPair(std::vector<FPrefixPrefillReading> Readings, FChunkPricePair& Out)
	{
		std::stable_sort(Readings.begin(), Readings.end(),
			[](const FPrefixPrefillReading& A, const FPrefixPrefillReading& B) { return A.StartDepth < B.StartDepth; });
		return FitPrefixPrefillPair(Readings.data(), Readings.size(), kPrefixPrefillFitGroups, Out);
	}

	// §15.32's attribution reading: the conventional median of the readings' ms per token (0 for none).
	inline double MedianMsPerToken(const FPrefixPrefillReading* Readings, size_t Count)
	{
		std::vector<double> V;
		for (size_t I = 0; I < Count; ++I)
		{
			if (Readings[I].Tokens > 0)
			{
				V.push_back(Readings[I].Ms / static_cast<double>(Readings[I].Tokens));
			}
		}
		if (V.empty())
		{
			return 0.0;
		}
		std::sort(V.begin(), V.end());
		const size_t N = V.size();
		return N % 2 == 1 ? V[N / 2] : 0.5 * (V[N / 2 - 1] + V[N / 2]);
	}

	// The estimate's rounding (D-SLM8113): ceil(x x 10^d - 1e-9) / 10^d, printed with exactly Decimals digits.
	inline std::string RoundUpDecimalText(double Value, int32_t Decimals)
	{
		const double Scale = std::pow(10.0, Decimals);
		const int64_t K = static_cast<int64_t>(std::ceil(Value * Scale - 1e-9));
		const int64_t Unit = static_cast<int64_t>(Scale + 0.5);
		const bool bNegative = K < 0;
		const int64_t Abs = bNegative ? -K : K;
		std::string Fraction = std::to_string(Abs % Unit);
		if (static_cast<int32_t>(Fraction.size()) < Decimals)
		{
			Fraction.insert(0, static_cast<size_t>(Decimals - static_cast<int32_t>(Fraction.size())), '0');
		}
		std::string Text = (bNegative ? "-" : "") + std::to_string(Abs / Unit);
		if (Decimals > 0)
		{
			Text += "." + Fraction;
		}
		return Text;
	}

	namespace IniEditDetail
	{
		inline std::string Trim(const std::string& S)
		{
			const char* const Space = " \t\r\n\v\f"; // TrimStartAndEnd's ASCII whitespace
			const size_t B = S.find_first_not_of(Space);
			if (B == std::string::npos)
			{
				return std::string();
			}
			const size_t E = S.find_last_not_of(Space);
			return S.substr(B, E - B + 1);
		}
	}

	// Text with Keys set in its SectionIndex-th section (counted from 0 over the lines that, trimmed, start with
	// '[', as SuperSLMRuntimeResolve::ParseIniSections counts them; a line ends at "\r\n", a lone '\r' or '\n', as
	// ParseIntoArrayLines breaks it; code review S1): every key=value line of that section whose
	// trimmed key is one of Keys is removed, and "key=value" lines for Keys, in order, follow the section's last
	// non-blank line. Every other line is kept as it was; lines end "\n". OutFound is false, and Text is returned
	// unchanged, when there is no such section.
	inline std::string SetIniSectionKeys(const std::string& Text, size_t SectionIndex,
		const std::vector<std::pair<std::string, std::string>>& Keys, bool& OutFound)
	{
		using IniEditDetail::Trim;
		OutFound = false;
		std::vector<std::string> Lines;
		size_t Start = 0;
		while (Start < Text.size())
		{
			const size_t End = Text.find_first_of("\r\n", Start);
			if (End == std::string::npos)
			{
				Lines.push_back(Text.substr(Start));
				break;
			}
			Lines.push_back(Text.substr(Start, End - Start));
			Start = End + ((Text[End] == '\r' && End + 1 < Text.size() && Text[End + 1] == '\n') ? 2 : 1);
		}
		size_t Section = 0;
		bool bInSection = false;
		bool bSeenAny = false;
		size_t Begin = 0, Finish = Lines.size();
		for (size_t I = 0; I < Lines.size(); ++I)
		{
			const std::string T = Trim(Lines[I]);
			if (!T.empty() && T[0] == '[')
			{
				if (bInSection)
				{
					Finish = I;
					break;
				}
				if (bSeenAny)
				{
					++Section;
				}
				bSeenAny = true;
				if (Section == SectionIndex)
				{
					bInSection = true;
					Begin = I + 1;
				}
			}
		}
		if (!bInSection)
		{
			return Text;
		}
		OutFound = true;
		std::vector<std::string> Kept;
		size_t LastContent = 0; // the index in Kept just after the section's header or last non-blank kept line
		for (size_t I = 0; I < Lines.size(); ++I)
		{
			if (I >= Begin && I < Finish)
			{
				const std::string T = Trim(Lines[I]);
				const size_t Eq = T.find('=');
				if (!T.empty() && T[0] != ';' && Eq != std::string::npos)
				{
					const std::string Key = Trim(T.substr(0, Eq));
					const bool bReplaced = std::any_of(Keys.begin(), Keys.end(),
						[&Key](const std::pair<std::string, std::string>& K) { return K.first == Key; });
					if (bReplaced)
					{
						continue;
					}
				}
				Kept.push_back(Lines[I]);
				if (!T.empty())
				{
					LastContent = Kept.size();
				}
				continue;
			}
			if (I == Begin - 1)
			{
				Kept.push_back(Lines[I]);
				LastContent = Kept.size();
				continue;
			}
			Kept.push_back(Lines[I]);
		}
		std::vector<std::string> Insert;
		for (const std::pair<std::string, std::string>& K : Keys)
		{
			Insert.push_back(K.first + "=" + K.second);
		}
		Kept.insert(Kept.begin() + static_cast<std::ptrdiff_t>(LastContent), Insert.begin(), Insert.end());
		std::string Out;
		for (const std::string& L : Kept)
		{
			Out += L;
			Out += "\n";
		}
		return Out;
	}
}
