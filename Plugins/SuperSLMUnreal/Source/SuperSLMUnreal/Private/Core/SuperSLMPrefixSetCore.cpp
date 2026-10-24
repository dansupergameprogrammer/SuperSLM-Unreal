#include "SuperSLMPrefixSetCore.h"

#include "superslm/sha256.h"

#include <map>

namespace SuperSLMCore
{
	namespace
	{
		const char* PrefixSetBackendLabel(uint8_t Backend)
		{
			return Backend == 0 ? "CPU" : "GPU";
		}

		void PrefixSetHex32(std::string& Out, uint32_t V)
		{
			static const char kDigits[] = "0123456789abcdef";
			Out += "0x";
			for (int Shift = 12; Shift >= 0; Shift -= 4)
			{
				Out.push_back(kDigits[(V >> static_cast<uint32_t>(Shift)) & 0xFu]);
			}
		}

		FPrefixSetFindingLine PrefixSetManifestLine(EPrefixSetFinding Finding, const FPrefixSetEntry& E, const std::string& Detail)
		{
			FPrefixSetFindingLine L;
			L.Finding = Finding;
			L.EntryName = E.Name;
			L.Detail = Detail;
			return L;
		}

		FPrefixSetFindingLine PrefixSetBuildLine(EPrefixSetFinding Finding, const std::string& Name, const FPrefixEngineIdentity& Producer,
			const std::string& Detail)
		{
			FPrefixSetFindingLine L;
			L.Finding = Finding;
			L.EntryName = Name;
			L.bHasProducer = true;
			L.Backend = Producer.Backend;
			L.AdapterVendorId = Producer.AdapterVendorId;
			L.AdapterDeviceId = Producer.AdapterDeviceId;
			L.Detail = Detail;
			return L;
		}

		// The first index of each name (the entry a parent name resolves to).
		std::map<std::string, size_t> PrefixSetFirstByName(const std::vector<FPrefixSetEntry>& Entries)
		{
			std::map<std::string, size_t> ByName;
			for (size_t I = 0; I < Entries.size(); ++I)
			{
				ByName.emplace(Entries[I].Name, I);
			}
			return ByName;
		}

		// Walks Index's parent chain. 0: resolves to a root; 1: an unknown parent on the chain; 2: a cycle.
		int PrefixSetChainStatus(const std::vector<FPrefixSetEntry>& Entries, const std::map<std::string, size_t>& ByName, size_t Index)
		{
			size_t At = Index;
			for (size_t Steps = 0; Steps <= Entries.size(); ++Steps)
			{
				const std::string& Parent = Entries[At].ParentName;
				if (Parent.empty())
				{
					return 0;
				}
				const auto It = ByName.find(Parent);
				if (It == ByName.end())
				{
					return 1;
				}
				At = It->second;
			}
			return 2;
		}

		// The entry's own malformed-spec finding, as RequestPrefix refuses one (§5.6).
		bool PrefixSetSpecMalformed(const FPrefixSetEntry& E, const FPrefixSetModelShape& Shape, EPrefixSetFinding& OutFinding, std::string& OutDetail)
		{
			if (E.Tokens.empty())
			{
				OutFinding = EPrefixSetFinding::EmptyTokens;
				OutDetail = "no token ids";
				return true;
			}
			for (size_t I = 0; I < E.Tokens.size(); ++I)
			{
				if (E.Tokens[I] < 0 || E.Tokens[I] >= Shape.VocabSize)
				{
					OutFinding = EPrefixSetFinding::TokenOutOfVocabulary;
					OutDetail = "token " + std::to_string(I) + " is " + std::to_string(E.Tokens[I]) + ", outside [0, " + std::to_string(Shape.VocabSize) + ")";
					return true;
				}
			}
			if (static_cast<int64_t>(E.Tokens.size()) > Shape.ContextCap)
			{
				OutFinding = EPrefixSetFinding::LongerThanContextCap;
				OutDetail = std::to_string(E.Tokens.size()) + " tokens, context_cap " + std::to_string(Shape.ContextCap);
				return true;
			}
			return false;
		}

		EPrefixSetFinding PrefixSetFindingOfCheck(EPrefixWrapperCheck Check)
		{
			switch (Check)
			{
			case EPrefixWrapperCheck::Layer1TagMismatch: return EPrefixSetFinding::StaleLayer1Tag;
			case EPrefixWrapperCheck::Layer1CommitMismatch: return EPrefixSetFinding::StaleLayer1Commit;
			case EPrefixWrapperCheck::ArtifactMismatch: return EPrefixSetFinding::StaleArtifact;
			case EPrefixWrapperCheck::KeyMismatch:
			case EPrefixWrapperCheck::SpecMismatch: return EPrefixSetFinding::SpecEdited;
			default: return EPrefixSetFinding::WrapperInvalid;
			}
		}

		bool PrefixSetSameProducer(const FPrefixEngineIdentity& A, const FPrefixEngineIdentity& B)
		{
			return PrefixSetBuildServes(A, B) && PrefixSetBuildServes(B, A);
		}
	}

	// CheckPrefixSetManifest, with each line's entry index (OutLineEntry, optional, one per line).
	std::vector<FPrefixSetFindingLine> CheckPrefixSetManifestOf(const std::vector<FPrefixSetEntry>& Entries,
		const FPrefixSetModelShape& Shape, std::vector<bool>* OutBuildable, std::vector<size_t>* OutLineEntry);

	const char* PrefixSetFindingName(EPrefixSetFinding Finding)
	{
		switch (Finding)
		{
		case EPrefixSetFinding::EmptyName: return "EmptyName";
		case EPrefixSetFinding::DuplicateName: return "DuplicateName";
		case EPrefixSetFinding::EmptyTokens: return "EmptyTokens";
		case EPrefixSetFinding::TokenOutOfVocabulary: return "TokenOutOfVocabulary";
		case EPrefixSetFinding::LongerThanContextCap: return "LongerThanContextCap";
		case EPrefixSetFinding::UnknownParent: return "UnknownParent";
		case EPrefixSetFinding::ParentCycle: return "ParentCycle";
		case EPrefixSetFinding::NestedRequiresBudgetMode: return "NestedRequiresBudgetMode";
		case EPrefixSetFinding::NotBuilt: return "NotBuilt";
		case EPrefixSetFinding::StaleLayer1Tag: return "StaleLayer1Tag";
		case EPrefixSetFinding::StaleLayer1Commit: return "StaleLayer1Commit";
		case EPrefixSetFinding::StaleArtifact: return "StaleArtifact";
		case EPrefixSetFinding::SpecEdited: return "SpecEdited";
		case EPrefixSetFinding::WrapperInvalid: return "WrapperInvalid";
		case EPrefixSetFinding::Orphaned: return "Orphaned";
		case EPrefixSetFinding::TokenizeFailed: return "TokenizeFailed";
		case EPrefixSetFinding::TextChanged: return "TextChanged";
		case EPrefixSetFinding::Count: break;
		}
		return "Unknown";
	}

	std::string FormatPrefixSetFinding(const std::string& SetName, const FPrefixSetFindingLine& Line)
	{
		std::string Out = SetName + ": entry '" + Line.EntryName + "'";
		if (Line.bHasProducer)
		{
			Out += " [";
			Out += PrefixSetBackendLabel(Line.Backend);
			if (Line.Backend != 0)
			{
				Out += " ";
				PrefixSetHex32(Out, Line.AdapterVendorId);
				Out += ":";
				PrefixSetHex32(Out, Line.AdapterDeviceId);
			}
			Out += "]";
		}
		Out += ": ";
		Out += PrefixSetFindingName(Line.Finding);
		if (!Line.Detail.empty())
		{
			Out += " (" + Line.Detail + ")";
		}
		return Out;
	}

	std::vector<FPrefixSetFindingLine> CheckPrefixSetManifest(const std::vector<FPrefixSetEntry>& Entries,
		const FPrefixSetModelShape& Shape, std::vector<bool>* OutBuildable)
	{
		return CheckPrefixSetManifestOf(Entries, Shape, OutBuildable, nullptr);
	}

	std::vector<FPrefixSetFindingLine> CheckPrefixSetManifestOf(const std::vector<FPrefixSetEntry>& Entries,
		const FPrefixSetModelShape& Shape, std::vector<bool>* OutBuildable, std::vector<size_t>* OutLineEntry)
	{
		std::vector<FPrefixSetFindingLine> Lines;
		std::vector<bool> Buildable(Entries.size(), false);
		const std::map<std::string, size_t> ByName = PrefixSetFirstByName(Entries);
		for (size_t I = 0; I < Entries.size(); ++I)
		{
			const FPrefixSetEntry& E = Entries[I];
			EPrefixSetFinding Finding = EPrefixSetFinding::Count;
			std::string Detail;
			if (E.Name.empty())
			{
				Finding = EPrefixSetFinding::EmptyName;
				Detail = "entry " + std::to_string(I) + " has no name";
			}
			else if (ByName.at(E.Name) != I)
			{
				Finding = EPrefixSetFinding::DuplicateName;
				Detail = "entry " + std::to_string(I) + " repeats entry " + std::to_string(ByName.at(E.Name)) + "'s name";
			}
			else if (PrefixSetSpecMalformed(E, Shape, Finding, Detail))
			{
			}
			else if (!E.ParentName.empty())
			{
				const int Chain = PrefixSetChainStatus(Entries, ByName, I);
				if (Chain == 1)
				{
					Finding = EPrefixSetFinding::UnknownParent;
					Detail = "no entry named '" + E.ParentName + "' on its parent chain";
				}
				else if (Chain == 2)
				{
					Finding = EPrefixSetFinding::ParentCycle;
					Detail = "its parent chain returns to itself";
				}
				else
				{
					Finding = EPrefixSetFinding::NestedRequiresBudgetMode;
					Detail = "nested cache entries wait for budget-mode nesting; the build leaves it empty and the runtime refuses the spec by name";
				}
			}
			if (Finding == EPrefixSetFinding::Count)
			{
				Buildable[I] = true;
			}
			else
			{
				Lines.push_back(PrefixSetManifestLine(Finding, E, Detail));
				if (OutLineEntry != nullptr)
				{
					OutLineEntry->push_back(I);
				}
			}
		}
		if (OutBuildable != nullptr)
		{
			*OutBuildable = Buildable;
		}
		return Lines;
	}

	void ComputePrefixSetKeys(const std::vector<FPrefixSetEntry>& Entries, const FPrefixEngineIdentity& Identity,
		std::vector<FPrefixKey>& OutKeys, std::vector<FPrefixStoredSpec>& OutSpecs, std::vector<bool>& OutHasKey)
	{
		const size_t N = Entries.size();
		OutKeys.assign(N, FPrefixKey());
		OutSpecs.assign(N, FPrefixStoredSpec());
		OutHasKey.assign(N, false);
		const std::map<std::string, size_t> ByName = PrefixSetFirstByName(Entries);
		// Resolves each entry after its parent; a chain that does not reach a root keeps no key.
		std::vector<int> State(N, 0); // 0 unvisited, 1 resolved, 2 unresolvable
		for (size_t Start = 0; Start < N; ++Start)
		{
			if (State[Start] != 0)
			{
				continue;
			}
			std::vector<size_t> Chain;
			size_t At = Start;
			bool bResolvable = true;
			while (true)
			{
				if (State[At] == 1)
				{
					break;
				}
				if (State[At] == 2 || Chain.size() > N)
				{
					bResolvable = false;
					break;
				}
				Chain.push_back(At);
				const std::string& Parent = Entries[At].ParentName;
				if (Parent.empty())
				{
					break;
				}
				const auto It = ByName.find(Parent);
				if (It == ByName.end())
				{
					bResolvable = false;
					break;
				}
				At = It->second;
			}
			// Chain runs child to ancestor; key them ancestor first.
			for (size_t K = Chain.size(); K-- > 0;)
			{
				const size_t I = Chain[K];
				if (!bResolvable)
				{
					State[I] = 2;
					continue;
				}
				FPrefixStoredSpec Spec;
				Spec.SchemaName = Entries[I].SchemaName;
				Spec.Tokens = Entries[I].Tokens;
				if (!Entries[I].ParentName.empty())
				{
					const size_t P = ByName.at(Entries[I].ParentName);
					Spec.bHasParent = true;
					Spec.ParentKey = OutKeys[P];
				}
				FPrefixKeyInputs Inputs;
				Inputs.ArtifactHash = Identity.ArtifactHash;
				Inputs.Layer1Tag = Identity.Layer1Tag;
				Inputs.Layer1Commit = Identity.Layer1Commit;
				Inputs.Backend = Identity.Backend;
				Inputs.AdapterVendorId = Identity.AdapterVendorId;
				Inputs.AdapterDeviceId = Identity.AdapterDeviceId;
				Inputs.SchemaName = Spec.SchemaName;
				Inputs.Tokens = Spec.Tokens;
				Inputs.bHasParent = Spec.bHasParent;
				Inputs.ParentKey = Spec.ParentKey;
				OutKeys[I] = ComputePrefixKey(Inputs);
				OutSpecs[I] = Spec;
				OutHasKey[I] = true;
				State[I] = 1;
			}
		}
	}

	FPrefixSetBuild AssemblePrefixSetBuild(const std::vector<FPrefixSetEntry>& Entries, const FPrefixEngineIdentity& Producer,
		const std::vector<std::vector<uint8_t>>& Payloads)
	{
		FPrefixSetBuild Build;
		Build.Producer = Producer;
		std::vector<FPrefixKey> Keys;
		std::vector<FPrefixStoredSpec> Specs;
		std::vector<bool> HasKey;
		ComputePrefixSetKeys(Entries, Producer, Keys, Specs, HasKey);
		Build.Entries.resize(Entries.size());
		for (size_t I = 0; I < Entries.size(); ++I)
		{
			Build.Entries[I].Name = Entries[I].Name;
			if (I < Payloads.size() && !Payloads[I].empty() && HasKey[I])
			{
				Build.Entries[I].Wrapper = EncodePrefixWrapper(Producer, Keys[I], Specs[I], Payloads[I].data(), Payloads[I].size());
			}
		}
		return Build;
	}

	bool PrefixSetBuildServes(const FPrefixEngineIdentity& BuildProducer, const FPrefixEngineIdentity& Runtime)
	{
		if (BuildProducer.Backend != Runtime.Backend)
		{
			return false;
		}
		if (BuildProducer.Backend == 0)
		{
			return true;
		}
		return BuildProducer.AdapterVendorId == Runtime.AdapterVendorId && BuildProducer.AdapterDeviceId == Runtime.AdapterDeviceId;
	}

	void ReplacePrefixSetBuild(std::vector<FPrefixSetBuild>& Builds, const FPrefixSetBuild& Build)
	{
		for (size_t I = Builds.size(); I-- > 0;)
		{
			if (PrefixSetSameProducer(Builds[I].Producer, Build.Producer))
			{
				Builds.erase(Builds.begin() + static_cast<std::ptrdiff_t>(I));
			}
		}
		Builds.push_back(Build);
	}

	std::vector<FPrefixSetFindingLine> ValidatePrefixSetForCook(const std::vector<FPrefixSetEntry>& Entries,
		const std::vector<FPrefixSetBuild>& Builds, const FPrefixEngineIdentity& Current, const FPrefixSetModelShape& Shape)
	{
		std::vector<bool> Buildable;
		std::vector<size_t> ManifestEntry;
		const std::vector<FPrefixSetFindingLine> Manifest = CheckPrefixSetManifestOf(Entries, Shape, &Buildable, &ManifestEntry);
		const std::map<std::string, size_t> ByName = PrefixSetFirstByName(Entries);
		// A Text entry whose ids are not its text's under this cook's model (§5.8 *Staleness*): one TextChanged
		// finding, in place of its manifest finding; the cook ships it with no ids and no bytes, so it is neither
		// checked against a build nor its bytes orphaned. A stale model takes this path, not StaleArtifact's.
		std::vector<bool> TextStale(Entries.size(), false);
		for (const size_t I : PrefixSetStaleTextEntries(Entries, Current.ArtifactHash))
		{
			TextStale[I] = true;
			Buildable[I] = false;
		}
		std::vector<FPrefixSetFindingLine> Lines;
		{
			size_t Next = 0; // manifest lines are in entry order, at most one per entry
			for (size_t I = 0; I < Entries.size(); ++I)
			{
				const bool bHasManifestLine = Next < Manifest.size() && ManifestEntry[Next] == I;
				if (TextStale[I])
				{
					const FPrefixSetEntry& E = Entries[I];
					std::string Detail;
					if (E.Tokens.empty())
					{
						Detail = "no ids tokenized from its text";
					}
					else if (E.TextHash != PrefixSetTextHash(E.Text))
					{
						Detail = "its text changed since its ids were built";
					}
					else
					{
						Detail = "its ids were tokenized under another model";
					}
					Lines.push_back(PrefixSetManifestLine(EPrefixSetFinding::TextChanged, E,
						Detail + "; the cook ships it with no ids and no bytes, and it is not served (rebuild the set)"));
				}
				else if (bHasManifestLine)
				{
					Lines.push_back(Manifest[Next]);
				}
				Next += bHasManifestLine ? 1 : 0;
			}
		}

		auto ValidateBuild = [&](const FPrefixSetBuild* Build, const FPrefixEngineIdentity& Producer)
		{
			// What the runtime will check these bytes against: this cook's pin and model, the build's producer.
			FPrefixEngineIdentity Expected = Current;
			Expected.Backend = Producer.Backend;
			Expected.AdapterVendorId = Producer.AdapterVendorId;
			Expected.AdapterDeviceId = Producer.AdapterDeviceId;
			std::vector<FPrefixKey> Keys;
			std::vector<FPrefixStoredSpec> Specs;
			std::vector<bool> HasKey;
			ComputePrefixSetKeys(Entries, Expected, Keys, Specs, HasKey);

			for (size_t I = 0; I < Entries.size(); ++I)
			{
				if (!Buildable[I])
				{
					continue; // the manifest's finding covers it
				}
				const FPrefixSetBuiltEntry* Built = nullptr;
				if (Build != nullptr)
				{
					for (const FPrefixSetBuiltEntry& B : Build->Entries)
					{
						if (B.Name == Entries[I].Name)
						{
							Built = &B;
							break;
						}
					}
				}
				if (Built == nullptr || Built->Wrapper.empty())
				{
					Lines.push_back(PrefixSetBuildLine(EPrefixSetFinding::NotBuilt, Entries[I].Name, Producer, "no bytes; the runtime recomputes it"));
					continue;
				}
				size_t Offset = 0;
				size_t Size = 0;
				const EPrefixWrapperCheck Check = CheckPrefixWrapper(Built->Wrapper.data(), Built->Wrapper.size(), Expected, Keys[I], Specs[I], Offset, Size);
				if (Check != EPrefixWrapperCheck::Ok)
				{
					Lines.push_back(PrefixSetBuildLine(PrefixSetFindingOfCheck(Check), Entries[I].Name, Producer,
						std::string("wrapper check ") + PrefixWrapperCheckName(Check) + "; the bytes ship and the runtime recomputes it"));
				}
			}
			if (Build == nullptr)
			{
				return;
			}
			std::map<std::string, int> Seen;
			for (const FPrefixSetBuiltEntry& B : Build->Entries)
			{
				const auto It = ByName.find(B.Name);
				if (It != ByName.end() && TextStale[It->second])
				{
					continue; // the cook clears these bytes (TextChanged above)
				}
				const bool bKnown = It != ByName.end() && Buildable[It->second];
				if (!B.Wrapper.empty() && (!bKnown || Seen[B.Name]++ > 0))
				{
					Lines.push_back(PrefixSetBuildLine(EPrefixSetFinding::Orphaned, B.Name, Producer, "bytes for no entry of the set; they ship and are never used"));
				}
			}
		};

		bool bHasCpu = false;
		for (const FPrefixSetBuild& Build : Builds)
		{
			bHasCpu = bHasCpu || Build.Producer.Backend == 0;
			ValidateBuild(&Build, Build.Producer);
		}
		if (!bHasCpu)
		{
			FPrefixEngineIdentity Cpu;
			Cpu.Backend = 0;
			ValidateBuild(nullptr, Cpu);
		}
		return Lines;
	}

	int32_t SelectPrefixSetBuild(const std::vector<FPrefixSetBuild>& Builds, const FPrefixEngineIdentity& Runtime)
	{
		for (size_t I = 0; I < Builds.size(); ++I)
		{
			if (PrefixSetBuildServes(Builds[I].Producer, Runtime))
			{
				return static_cast<int32_t>(I);
			}
		}
		return -1;
	}

	std::vector<FPrefixSetRegistration> PrefixSetRegistrations(const FPrefixSetBuild& Build)
	{
		std::vector<FPrefixSetRegistration> Out;
		for (size_t I = 0; I < Build.Entries.size(); ++I)
		{
			const std::vector<uint8_t>& W = Build.Entries[I].Wrapper;
			FPrefixSetRegistration R;
			if (!W.empty() && ReadPrefixWrapperKey(W.data(), W.size(), R.Key))
			{
				R.BuiltEntryIndex = I;
				Out.push_back(R);
			}
		}
		return Out;
	}

	bool PrefixSetModelMatches(const std::array<uint8_t, 32>& SetModelArtifactHash,
		const std::array<uint8_t, 32>& ConfiguredModelArtifactHash)
	{
		return SetModelArtifactHash == ConfiguredModelArtifactHash;
	}

	const char* PrefixSetSpecResultName(EPrefixSetSpecResult Result)
	{
		switch (Result)
		{
		case EPrefixSetSpecResult::Ok: return "Ok";
		case EPrefixSetSpecResult::PrefixSetModelMismatch: return "PrefixSetModelMismatch";
		case EPrefixSetSpecResult::UnknownEntry: return "UnknownEntry";
		case EPrefixSetSpecResult::NoStoredIds: return "NoStoredIds";
		case EPrefixSetSpecResult::TextChanged: return "TextChanged";
		}
		return "Unknown";
	}

	EPrefixSetSpecResult FindPrefixSetEntrySpec(const std::vector<FPrefixSetEntry>& Entries,
		const std::array<uint8_t, 32>& SetModelArtifactHash, const std::array<uint8_t, 32>& ConfiguredModelArtifactHash,
		const std::string& EntryName, std::vector<int32_t>& OutTokens, std::string& OutSchemaName, std::string& OutParentEntry)
	{
		OutTokens.clear();
		OutSchemaName.clear();
		OutParentEntry.clear();
		if (!PrefixSetModelMatches(SetModelArtifactHash, ConfiguredModelArtifactHash))
		{
			return EPrefixSetSpecResult::PrefixSetModelMismatch;
		}
		for (const FPrefixSetEntry& Entry : Entries)
		{
			if (Entry.Name != EntryName)
			{
				continue;
			}
			if (Entry.Tokens.empty())
			{
				return EPrefixSetSpecResult::NoStoredIds;
			}
			if (!PrefixSetTextEntryCurrent(Entry, SetModelArtifactHash))
			{
				return EPrefixSetSpecResult::TextChanged;
			}
			OutTokens = Entry.Tokens;
			OutSchemaName = Entry.SchemaName;
			OutParentEntry = Entry.ParentName;
			return EPrefixSetSpecResult::Ok;
		}
		return EPrefixSetSpecResult::UnknownEntry;
	}

	std::array<uint8_t, 32> PrefixSetTextHash(const std::string& Utf8Text)
	{
		std::array<uint8_t, 32> Out{};
		superslm::Sha256Hash(reinterpret_cast<const uint8_t*>(Utf8Text.data()), Utf8Text.size(), Out.data());
		return Out;
	}

	bool PrefixSetTextEntryCurrent(const FPrefixSetEntry& Entry, const std::array<uint8_t, 32>& ModelArtifactHash)
	{
		if (Entry.Input != EPrefixSetEntryInput::Text)
		{
			return true;
		}
		return !Entry.Tokens.empty() && Entry.TokenizedModelHash == ModelArtifactHash && Entry.TextHash == PrefixSetTextHash(Entry.Text);
	}

	FPrefixSetTokenizePass TokenizePrefixSetText(const std::vector<FPrefixSetEntry>& Entries, const FPrefixSetModelShape& Shape,
		const std::array<uint8_t, 32>& ModelArtifactHash, const FPrefixSetTokenizeFn& Tokenize)
	{
		FPrefixSetTokenizePass Pass;
		Pass.Entries = Entries;
		Pass.Failed.assign(Entries.size(), false);
		FPrefixEngineIdentity Cpu;
		Cpu.Backend = 0;
		for (size_t I = 0; I < Pass.Entries.size(); ++I)
		{
			FPrefixSetEntry& E = Pass.Entries[I];
			if (E.Input != EPrefixSetEntryInput::Text)
			{
				continue;
			}
			EPrefixSetFinding Finding = EPrefixSetFinding::Count;
			std::string Detail;
			std::vector<int32_t> Ids;
			if (E.Text.empty())
			{
				Finding = EPrefixSetFinding::EmptyTokens;
				Detail = "its text is empty";
			}
			else
			{
				std::string Error;
				++Pass.Calls;
				if (!Tokenize(E.Text, Ids, Error))
				{
					Finding = EPrefixSetFinding::TokenizeFailed;
					Detail = Error.empty() ? std::string("the tokenizer refused its text") : Error;
				}
				else if (Ids.empty())
				{
					Finding = EPrefixSetFinding::EmptyTokens;
					Detail = "its text tokenized to no ids";
				}
				else if (static_cast<int64_t>(Ids.size()) > Shape.ContextCap)
				{
					Finding = EPrefixSetFinding::LongerThanContextCap;
					Detail = "its text tokenized to " + std::to_string(Ids.size()) + " ids, context_cap " + std::to_string(Shape.ContextCap);
				}
			}
			if (Finding != EPrefixSetFinding::Count)
			{
				// Per-entry failure: the entry keeps its previous ids and hashes, and the build keeps its bytes.
				Pass.Failed[I] = true;
				Pass.Lines.push_back(PrefixSetBuildLine(Finding, E.Name, Cpu, Detail + "; nothing built for it, it keeps its previous ids and bytes"));
				continue;
			}
			E.Tokens = std::move(Ids);
			E.TextHash = PrefixSetTextHash(E.Text);
			E.TokenizedModelHash = ModelArtifactHash;
		}
		return Pass;
	}

	void KeepPreviousPrefixSetBytes(FPrefixSetBuild& Built, const std::vector<FPrefixSetBuild>& Previous, const std::vector<bool>& Kept)
	{
		const FPrefixSetBuild* Before = nullptr;
		for (const FPrefixSetBuild& B : Previous)
		{
			if (PrefixSetSameProducer(B.Producer, Built.Producer))
			{
				Before = &B;
				break;
			}
		}
		for (size_t I = 0; I < Built.Entries.size() && I < Kept.size(); ++I)
		{
			if (!Kept[I])
			{
				continue;
			}
			Built.Entries[I].Wrapper.clear();
			if (Before == nullptr)
			{
				continue;
			}
			for (const FPrefixSetBuiltEntry& Old : Before->Entries)
			{
				if (Old.Name == Built.Entries[I].Name)
				{
					Built.Entries[I].Wrapper = Old.Wrapper;
					break;
				}
			}
		}
	}

	std::vector<size_t> PrefixSetStaleTextEntries(const std::vector<FPrefixSetEntry>& Entries, const std::array<uint8_t, 32>& ModelArtifactHash)
	{
		std::vector<size_t> Out;
		for (size_t I = 0; I < Entries.size(); ++I)
		{
			if (!PrefixSetTextEntryCurrent(Entries[I], ModelArtifactHash))
			{
				Out.push_back(I);
			}
		}
		return Out;
	}

	void StripPrefixSetEntriesForCook(std::vector<FPrefixSetEntry>& Entries, std::vector<FPrefixSetBuild>& Builds, const std::vector<size_t>& Stale)
	{
		for (const size_t I : Stale)
		{
			if (I >= Entries.size())
			{
				continue;
			}
			Entries[I].Tokens.clear();
			for (FPrefixSetBuild& B : Builds)
			{
				for (FPrefixSetBuiltEntry& E : B.Entries)
				{
					if (E.Name == Entries[I].Name)
					{
						E.Wrapper.clear();
					}
				}
			}
		}
	}

	std::vector<FPrefixSetRegistration> PrefixSetRegistrations(const FPrefixSetBuild& Build, const std::vector<FPrefixSetEntry>& Entries,
		const std::array<uint8_t, 32>& SetModelArtifactHash)
	{
		std::map<std::string, bool> Stale;
		for (const size_t I : PrefixSetStaleTextEntries(Entries, SetModelArtifactHash))
		{
			Stale[Entries[I].Name] = true;
		}
		std::vector<FPrefixSetRegistration> Out;
		for (const FPrefixSetRegistration& R : PrefixSetRegistrations(Build))
		{
			if (Stale.find(Build.Entries[R.BuiltEntryIndex].Name) == Stale.end())
			{
				Out.push_back(R);
			}
		}
		return Out;
	}
}
