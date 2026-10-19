static FSuperSLMConfigureReport PrepareCpuState(const SuperSLMRuntime::FModelBytes* Model, const FSuperSLMRuntimeConfig& Config,
	const FSuperSLMResolvedCpuConfig& Resolved, FSuperSLMSubsystemState*& OutState)
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR_ON_CHANNEL("SuperSLM.PoolAlloc", SuperSLMChannel);
	OutState = nullptr;

	FSuperSLMConfigureReport Report;
	auto Refuse = [&Report](ESuperSLMConfigureResult Result, const FString& Message)
	{
		Report.Result = Result;
		Report.Message = Message;
		UE_LOG(LogSuperSLM, Warning, TEXT("Configure refused: %s"), *Message);
		return Report;
	};

	if (Config.BlockCount <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidBlockCount, FString::Printf(
			TEXT("block_count must be at least 1 (got %d): it is the number of sequences the KV pool keeps resident at once"), Config.BlockCount));
	}
	if (Config.PrefixBlockCount < 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidBlockCount, FString::Printf(
			TEXT("PrefixBlockCount must be 0 or more (got %d): it is the number of shared-prefix blocks the KV pool reserves"), Config.PrefixBlockCount));
	}
	// The pool is BlockCount + PrefixBlockCount blocks (below); the sum is taken in int64 so a pin
	// near INT32_MAX is refused by name here rather than wrapping into an unnamed sizing refusal.
	if (static_cast<int64>(Config.BlockCount) + static_cast<int64>(Config.PrefixBlockCount) > static_cast<int64>(MAX_int32))
	{
		return Refuse(ESuperSLMConfigureResult::InvalidBlockCount, FString::Printf(
			TEXT("block_count (BlockCount %d) + PrefixBlockCount (%d) = %lld exceeds %d: the KV pool holds both, so lower one of them"),
			Config.BlockCount, Config.PrefixBlockCount, static_cast<int64>(Config.BlockCount) + static_cast<int64>(Config.PrefixBlockCount), MAX_int32));
	}
	if (Config.MaxSequencesPerDecodeCall <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("max_batch (MaxSequencesPerDecodeCall) must be at least 1 (got %d)"), Config.MaxSequencesPerDecodeCall));
	}
	if (Config.MaxPrefillChunkBudget <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("max_chunk_budget (MaxPrefillChunkBudget) must be at least 1 (got %d)"), Config.MaxPrefillChunkBudget));
	}
	if (Config.MaxLayerBudget <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("max_layer_budget (MaxLayerBudget) must be at least 1 (got %d)"), Config.MaxLayerBudget));
	}
	// Slice 1P (tiled-matmul plan §4A.1 item 1): the chunk floor is a count of tokens; 1 is 1.0's
	// prefill, and nothing below 1 means anything.
	if (Config.MinPrefillChunk <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("MinPrefillChunk must be at least 1 (got %d); 1 restores one-token prefill jobs, 8 reaches the tiled GEMM"), Config.MinPrefillChunk));
	}
	if (Config.MaxConcurrentCalls <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("MaxConcurrentCalls must be at least 1 (got %d)"), Config.MaxConcurrentCalls));
	}
	// D-SLM7799 rule 1: a non-finite TickBudgetMs (NaN, +inf or -inf) is refused like a non-positive one.
	if (!FMath::IsFinite(Config.TickBudgetMs) || Config.TickBudgetMs <= 0.0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("TickBudgetMs must be a finite, positive number of milliseconds (got %f)"), Config.TickBudgetMs));
	}
	if (!(Config.BudgetHeadroom > 0.0 && Config.BudgetHeadroom <= 1.0))
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("BudgetHeadroom must lie in (0, 1] (got %.3f)"), Config.BudgetHeadroom));
	}
	if (Config.MaxQueuedOperationsPerSequence <= 0)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("MaxQueuedOperationsPerSequence must be at least 1 (got %d)"), Config.MaxQueuedOperationsPerSequence));
	}
	if (!SuperSLMFinishHook::IsValidTaskCount(Resolved.ParallelTasks))
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("FinishParallelTasks must lie in [0, %d] (got %d): it is the most tasks one token finish, one decode projection, or one prefill GEMM is split into; 0 or 1 installs no hook"),
			SuperSLMFinishHook::MaxFinishParallelTasks, Resolved.ParallelTasks));
	}
	// T-2999 S1: the consumer refuses a cost the scheduler cannot size K from, whatever produced
	// it (a caller's config, a saved estimate the resolver accepted, or a hand edit) -- a
	// non-finite or non-positive per-unit cost is refused by name.
	{
		const TPair<const TCHAR*, double> Costs[] = {
			{ TEXT("LayerCostMs"), Resolved.LayerCostMs }, { TEXT("FinishCostMs"), Resolved.FinishCostMs },
			{ TEXT("PromptTokenCostMs"), Resolved.PromptTokenCostMs }, { TEXT("ResetCostMs"), Resolved.ResetCostMs },
			{ TEXT("AdoptCostMs"), Resolved.AdoptCostMs }, { TEXT("SaveCostMs"), Resolved.SaveCostMs },
			{ TEXT("RestoreCostMs"), Resolved.RestoreCostMs }, { TEXT("PrefixBeginCostMs"), Resolved.PrefixBeginCostMs },
			{ TEXT("PrefixReleaseCostMs"), Resolved.PrefixReleaseCostMs },
			// Slice 1P (plan §4A.1 item 2, rev 11.1 audit F6): the batched pair, refused as the
			// chunk-1 pair is.
			{ TEXT("BatchedPromptTokenCostMs"), Resolved.BatchedPromptTokenCostMs },
		};
		for (const TPair<const TCHAR*, double>& Cost : Costs)
		{
			if (!FMath::IsFinite(Cost.Value) || Cost.Value <= 0.0)
			{
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("%s must be a finite, positive number of milliseconds (got %f)"), Cost.Key, Cost.Value));
			}
		}
		// D-SLM7793/D-SLM7794: the per-position depth terms may be 0 (a flat cost), never
		// negative or non-finite.
		const TPair<const TCHAR*, double> Slopes[] = {
			{ TEXT("LayerCostPerPositionMs"), Resolved.LayerCostPerPositionMs },
			{ TEXT("PromptTokenCostPerPositionMs"), Resolved.PromptTokenCostPerPositionMs },
			{ TEXT("BatchedPromptTokenCostPerPositionMs"), Resolved.BatchedPromptTokenCostPerPositionMs },
		};
		for (const TPair<const TCHAR*, double>& Slope : Slopes)
		{
			if (!FMath::IsFinite(Slope.Value) || Slope.Value < 0.0)
			{
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("%s must be a finite, non-negative number of milliseconds per position (got %f)"), Slope.Key, Slope.Value));
			}
		}
	}
	if (Model == nullptr)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidModel, TEXT("no model was given"));
	}

	SuperSLMRuntime::FModelShape Shape;
	FString Error;
	if (!SuperSLMRuntime::ReadModelShape(*Model, Shape, Error))
	{
		return Refuse(ESuperSLMConfigureResult::InvalidModel, FString::Printf(TEXT("model %s: %s"), *Model->Name, *Error));
	}
	if (Config.MaxLayerBudget > Shape.NumHiddenLayers)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("max_layer_budget %d exceeds the model's num_hidden_layers (%d)"), Config.MaxLayerBudget, Shape.NumHiddenLayers));
	}
	// D-SLM7799 rule 3: the derived domain, now that the model shape is known. The resolver has
	// already replaced a tripping saved estimate with the mode profile where that clears it
	// (D-SLM8145, D-SLM8167); this is the final set's check, and it refuses as before.
	{
		FString DomainError;
		if (!SuperSLMRuntimeResolve::CheckCostDomain(Config, Resolved, Shape.ContextCap, DomainError))
		{
			return Refuse(ESuperSLMConfigureResult::InvalidCostDomain, DomainError);
		}
	}
	// Plugin paging plan §3.1 (cell 5): SequenceBudget is 0 (legacy mode) or a budget in
	// [1, context_cap]; anything else is refused by name before anything is allocated.
	if (Config.SequenceBudget < 0 || static_cast<int64>(Config.SequenceBudget) > Shape.ContextCap)
	{
		return Refuse(ESuperSLMConfigureResult::InvalidSequenceBudget, FString::Printf(
			TEXT("SequenceBudget must lie in [0, %lld] (got %d): 0 keeps legacy mode, and a budget-mode sequence may write at most the model's context_cap (%lld) positions"),
			Shape.ContextCap, Config.SequenceBudget, Shape.ContextCap));
	}
	const bool bBudgetMode = Config.SequenceBudget > 0;
	// Slice 1P (tiled-matmul plan §4A.1 item 5): a warning, never a refusal. A prefill call of fewer
	// than kTiledMinTokens tokens runs the engine's serial GEMM, so a call shape or floor below it
	// gives up the tiled kernel. Every shipped call shape uses 64 or 512.
	if (Config.MaxPrefillChunkBudget < SuperSLMPrefillPlanning::kTiledMinTokens || Config.MinPrefillChunk < SuperSLMPrefillPlanning::kTiledMinTokens)
	{
		UE_LOG(LogSuperSLM, Warning, TEXT("SuperSLM Configure: MaxPrefillChunkBudget %d, MinPrefillChunk %d: prefill will not reach the tiled GEMM (a prefill call needs at least %d tokens); tokens are unchanged."),
			Config.MaxPrefillChunkBudget, Config.MinPrefillChunk, SuperSLMPrefillPlanning::kTiledMinTokens);
	}

	FSuperSLMSubsystemState* NewState = new FSuperSLMSubsystemState();
	NewState->Shape = Shape;
	NewState->Config = Config;
	NewState->Resolved = Resolved;
	if (!SuperSLMRuntime::ReadArtifactHash(*Model, NewState->ArtifactHash))
	{
		delete NewState;
		return Refuse(ESuperSLMConfigureResult::InvalidModel, TEXT("the model carries no artifact header"));
	}
	NewState->Model = SuperSLMRuntime::AcquireModelMapping(*Model, Error);
	if (NewState->Model == nullptr)
	{
		delete NewState;
		return Refuse(ESuperSLMConfigureResult::InvalidModel, FString::Printf(TEXT("model %s: %s"), *Model->Name, *Error));
	}

	// From here the state owns a mapping reference; every failure below unwinds through
	// DestroyCpuState() (TearDown()'s own body), which releases it.
	FSuperSLMSubsystemState& S = *NewState;
	auto TearDown = [NewState]() { DestroyCpuState(NewState); };

	// Plugin paging plan §3.2 (step K2): budget mode reads the page geometry from the engine on
	// this thread (the mapping is held) and sizes the page ledger's regions from it, before the
	// lifecycle budget, which prices a budget holder's reservation (§3.3 "Lifecycle pricing").
	if (bBudgetMode)
	{
		S.PagePositions = static_cast<int64>(sslm_kv_page_positions(S.Model));
		S.PageBytes = static_cast<uint64>(sslm_kv_page_size(S.Model));
		SuperSLMPaging::FPageLedgerConfig LedgerConfig;
		LedgerConfig.PagePositions = S.PagePositions;
		LedgerConfig.PageBytes = S.PageBytes;
		LedgerConfig.ContextCap = Shape.ContextCap;
		LedgerConfig.EngineContextCap = 0; // the CPU's two caps are one: Cp = Ce (§3.2 "Two caps")
		LedgerConfig.SequenceBudget = Config.SequenceBudget;
		LedgerConfig.BlockCount = Config.BlockCount;
		LedgerConfig.PrefixBlockCount = Config.PrefixBlockCount;
		LedgerConfig.RestoreHeadroomBlocks = Config.RestoreHeadroomBlocks;
		LedgerConfig.CacheGhostBlocks = Config.CacheGhostBlocks;
		LedgerConfig.bCacheConfigured = false; // no prefix cache in the paging release: A and C are 0
		LedgerConfig.CacheRegionPages = 0;
		const SuperSLMPaging::ELedgerConfigure LedgerResult = S.Ledger.Configure(LedgerConfig);
		switch (LedgerResult)
		{
			case SuperSLMPaging::ELedgerConfigure::Ok:
				break;
			case SuperSLMPaging::ELedgerConfigure::InvalidSequenceBudget:
				TearDown();
				return Refuse(ESuperSLMConfigureResult::InvalidSequenceBudget, FString::Printf(
					TEXT("SequenceBudget %d is outside [1, context_cap %lld]"), Config.SequenceBudget, Shape.ContextCap));
			case SuperSLMPaging::ELedgerConfigure::InvalidGeometry:
				TearDown();
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("the page ledger refused the geometry (B %lld, page %llu bytes, context_cap %lld, BlockCount %d, PrefixBlockCount %d, RestoreHeadroomBlocks %d, CacheGhostBlocks %d): every count must be 0 or more and the pool must fit 2^32 - 1 pages"),
					S.PagePositions, S.PageBytes, Shape.ContextCap, Config.BlockCount, Config.PrefixBlockCount, Config.RestoreHeadroomBlocks, Config.CacheGhostBlocks));
			case SuperSLMPaging::ELedgerConfigure::LegacyMode:
				TearDown();
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("the page ledger read SequenceBudget %d as legacy mode (plugin defect)"), Config.SequenceBudget));
		}
	}

	// §5: the Sequence Lifecycle Budget is checked BEFORE anything is allocated. The same figure
	// SuperSLMSequenceLifecycleBudget::CheckModel() computes -- Layer 1's block size on the shared
	// mapping, over this box's measured bandwidths -- read from the mapping already held. Plugin
	// paging plan §3.3 "Lifecycle pricing" (cell 7.1): in budget mode a create or reset is priced
	// at R(SequenceBudget) x page_bytes, the reservation it touches, in place of the block size.
	const uint64 BudgetPricedBytes = bBudgetMode
		? SuperSLMPaging::BudgetModeLifecyclePricedBytes(S.PagePositions, Shape.ContextCap, Config.SequenceBudget, S.PageBytes)
		: 0;
	const int64 LifecyclePricedBytes = bBudgetMode ? static_cast<int64>(BudgetPricedBytes) : static_cast<int64>(sslm_kv_block_size(S.Model));
	double WriteBytesPerSec = 0.0;
	double AdoptToResetRatio = 0.0;
	SuperSLMSequenceLifecycleBudget::MeasureHostBandwidthAndAdoptRatio(WriteBytesPerSec, AdoptToResetRatio);
	const FSuperSLMSequenceLifecycleReport Lifecycle = SuperSLMSequenceLifecycleBudget::Predict(
		LifecyclePricedBytes, WriteBytesPerSec, AdoptToResetRatio, Config.SequenceLifecycleBudgetMs);
	if (!Lifecycle.bWithinBudget)
	{
		const FString Message = bBudgetMode
			? FString::Printf(
				TEXT("predicted reset %.2f ms / adopt %.2f ms for a %lld-byte KV reservation (R(SequenceBudget %d) = %lld pages of %llu bytes) exceed the Sequence Lifecycle Budget of %.2f ms. ")
				TEXT("The reservation grows with SequenceBudget: lower it, or raise the budget."),
				Lifecycle.PredictedResetMs, Lifecycle.PredictedAdoptMs, Lifecycle.KvBlockSizeBytes, Config.SequenceBudget,
				S.PageBytes > 0 ? static_cast<int64>(BudgetPricedBytes / S.PageBytes) : int64(0), S.PageBytes, Config.SequenceLifecycleBudgetMs)
			: FString::Printf(
				TEXT("predicted reset %.2f ms / adopt %.2f ms for a %lld-byte KV block exceed the Sequence Lifecycle Budget of %.2f ms. ")
				TEXT("The block size is linear in context_cap (%lld for this model): reconvert at a smaller context_cap, or raise the budget."),
				Lifecycle.PredictedResetMs, Lifecycle.PredictedAdoptMs, Lifecycle.KvBlockSizeBytes, Config.SequenceLifecycleBudgetMs, Shape.ContextCap);
		TearDown();
		return Refuse(ESuperSLMConfigureResult::SequenceLifecycleBudgetExceeded, Message);
	}

	// §4: sizing is a function of the artifact and the declared call shape.
	sslm_config CallShape;
	CallShape.max_batch = Config.MaxSequencesPerDecodeCall;
	CallShape.max_chunk_budget = Config.MaxPrefillChunkBudget;
	CallShape.max_layer_budget = Config.MaxLayerBudget;
	CallShape.reserved = 0;

	// The pool holds the warm sequences plus the shared-prefix blocks (D-SLM7342). The sum is
	// computed in int64; the range check above refused anything past INT32_MAX.
	const int64 PoolBlocks64 = static_cast<int64>(Config.BlockCount) + static_cast<int64>(Config.PrefixBlockCount);
	check(PoolBlocks64 > 0 && PoolBlocks64 <= static_cast<int64>(MAX_int32));
	const int32 PoolBlocks = static_cast<int32>(PoolBlocks64);
	const size_t BlockBytes = sslm_kv_block_size(S.Model);
	const size_t OverheadBytes = bBudgetMode ? 0 : sslm_kv_pool_overhead_size(S.Model, static_cast<uint32_t>(PoolBlocks));
	const size_t WorkspaceBytes = sslm_workspace_size(S.Model, &CallShape);
	const size_t StateBytes = sslm_seq_state_size(S.Model);
	if (BlockBytes == 0 || WorkspaceBytes == 0 || StateBytes == 0 ||
		(!bBudgetMode && (OverheadBytes == SIZE_MAX || BlockBytes > (SIZE_MAX - OverheadBytes) / static_cast<size_t>(PoolBlocks))))
	{
		TearDown();
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, TEXT("Layer 1 could not size the pool/workspace for this model and call shape"));
	}

	// Plugin paging plan §3.2: in budget mode the pool is a page pool of exactly the ledger's
	// S + P + H (+ A + C, 0 here) pages, so the engine's own KV_POOL_EXHAUSTED is reachable only
	// past a charge the ledger got wrong (a ledger fault, §3.7).
	const uint32 PoolPages = bBudgetMode ? S.Ledger.PoolPages() : 0u;
	const size_t PageBytes = bBudgetMode ? static_cast<size_t>(S.PageBytes) : 0;
	const size_t PageOverheadBytes = bBudgetMode ? sslm_kv_page_pool_overhead_size(S.Model, PoolPages) : 0;
	if (bBudgetMode && (PoolPages == 0 || PageBytes == 0 || PageOverheadBytes == SIZE_MAX ||
		PageBytes > (SIZE_MAX - PageOverheadBytes) / static_cast<size_t>(PoolPages)))
	{
		TearDown();
		return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
			TEXT("Layer 1 could not size a page pool of %u pages of %llu bytes for this model"), PoolPages, static_cast<uint64>(PageBytes)));
	}

	auto AllocFail = [&TearDown, &Refuse](const FString& What)
	{
		TearDown();
		return Refuse(ESuperSLMConfigureResult::AllocationFailed, What);
	};

	// Legacy: KV = sslm_kv_block_size * blocks + sslm_kv_pool_overhead_size (§4), 64-byte aligned.
	// Budget mode: pages * sslm_kv_page_size + sslm_kv_page_pool_overhead_size, as
	// sslm_kv_page_pool_create requires.
	S.KvBytes = bBudgetMode
		? static_cast<int64>(PageBytes * static_cast<size_t>(PoolPages) + PageOverheadBytes)
		: static_cast<int64>(BlockBytes * static_cast<size_t>(PoolBlocks) + OverheadBytes);
	S.KvBuffer = FMemory::Malloc(static_cast<SIZE_T>(S.KvBytes), SSLM_ABI_ALIGNMENT_BYTES);
	if (S.KvBuffer == nullptr)
	{
		return AllocFail(FString::Printf(TEXT("could not allocate the %lld-byte KV pool"), S.KvBytes));
	}
	sslm_status Status = bBudgetMode
		? sslm_kv_page_pool_create(S.Model, S.KvBuffer, static_cast<size_t>(S.KvBytes), PoolPages, &S.Pool)
		: sslm_kv_pool_create(S.Model, S.KvBuffer, static_cast<size_t>(S.KvBytes), static_cast<uint32_t>(PoolBlocks), &S.Pool);
	if (Status != SSLM_OK)
	{
		return AllocFail(FString::Printf(TEXT("%s failed (%s)"), bBudgetMode ? TEXT("sslm_kv_page_pool_create") : TEXT("sslm_kv_pool_create"), *StatusText(Status)));
	}

	// §4, D-SLM7341/D-SLM7407: one persistent worker thread and one workspace per lane; lanes =
	// min(MaxConcurrentCalls, BlockCount).
	const int32 LaneCount = FMath::Clamp(Config.MaxConcurrentCalls, 1, Config.BlockCount);
	S.WorkspaceBytesEach = static_cast<int64>(WorkspaceBytes);
	S.Lanes.SetNum(LaneCount);
	for (int32 I = 0; I < LaneCount; ++I)
	{
		FLane& Lane = S.Lanes[I];
		Lane.WorkspaceBuffer = FMemory::Malloc(WorkspaceBytes, SSLM_ABI_ALIGNMENT_BYTES);
		if (Lane.WorkspaceBuffer == nullptr)
		{
			return AllocFail(FString::Printf(TEXT("could not allocate a %llu-byte workspace"), static_cast<uint64>(WorkspaceBytes)));
		}
		Status = sslm_workspace_create(S.Model, &CallShape, Lane.WorkspaceBuffer, WorkspaceBytes, &Lane.Workspace);
		if (Status != SSLM_OK)
		{
			return AllocFail(FString::Printf(TEXT("sslm_workspace_create failed (%s)"), *StatusText(Status)));
		}
		// Plan §2.5 row 2 (D-SLM7663): the finish hook, installed right after the workspace is
		// created and before this lane's worker thread starts, so it never runs concurrently with
		// a call on the workspace.
		// The resolved decode mode rides on this hook (D-SLM8166): at 0 or 1 task none is installed,
		// and the effective mode is then off.
		if (SuperSLMFinishHook::ShouldInstall(Resolved.ParallelTasks))
		{
			const sslm_parallel_for Hook = SuperSLMFinishHook::Make(Resolved.ParallelTasks,
				Resolved.bDecodeModeEffective ? SSLM_PARALLEL_FOR_MATVEC : 0);
			Status = sslm_workspace_set_parallel_for(Lane.Workspace, &Hook);
			if (Status != SSLM_OK)
			{
				TearDown();
				return Refuse(ESuperSLMConfigureResult::InvalidCallShape, FString::Printf(
					TEXT("sslm_workspace_set_parallel_for refused the finish hook at FinishParallelTasks %d (%s)"), Resolved.ParallelTasks, *StatusText(Status)));
			}
		}
		// §4: save buffers are sized by sslm_seq_state_size (an upper bound) and created at
		// init, one per lane (plan §2.5 row 22).
		Lane.SaveStaging = FMemory::Malloc(StateBytes, SSLM_ABI_ALIGNMENT_BYTES);
		if (Lane.SaveStaging == nullptr)
		{
			return AllocFail(FString::Printf(TEXT("could not allocate a %llu-byte save buffer"), static_cast<uint64>(StateBytes)));
		}
		Lane.Runnable = new FSuperSLMCpuWorkerRunnable();
		Lane.Runnable->WakeEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false);
		Lane.Thread = FRunnableThread::Create(Lane.Runnable, *FString::Printf(TEXT("SuperSLM CPU Worker %d"), I));
		if (Lane.Thread == nullptr)
		{
			return AllocFail(FString::Printf(TEXT("could not create worker thread %d"), I));
		}
	}

	S.SaveStagingBytes = static_cast<int64>(StateBytes);

	// §5: the warm pool -- every sequence block_count allows is created now, so create and
	// release never run on a gameplay tick.
	S.Slots.SetNum(Config.BlockCount);
	for (int32 I = 0; I < Config.BlockCount; ++I)
	{
		// Plugin paging plan §3.2: in budget mode each warm holder reserves R(SequenceBudget)
		// pages, the slot's share of region S, which the ledger charged in full at its Configure.
		Status = bBudgetMode
			? sslm_seq_create_budgeted(S.Model, &S.Pool, Config.SequenceBudget, &S.Slots[I].Seq)
			: sslm_seq_create(S.Model, &S.Pool, &S.Slots[I].Seq);
		if (Status != SSLM_OK)
		{
			return AllocFail(FString::Printf(TEXT("%s failed for warm sequence %d of %d (%s)"),
				bBudgetMode ? TEXT("sslm_seq_create_budgeted") : TEXT("sslm_seq_create"), I + 1, Config.BlockCount, *StatusText(Status)));
		}
		S.Slots[I].Prompt.Reserve(Config.MaxPrefillChunkBudget);
		S.FreeSlots.Add(I);
	}
	S.IdToSlot.Reserve(Config.BlockCount);
	S.Prefixes.Reserve(Config.PrefixBlockCount);

	// The job ledger and the tick history: fixed rings, created here so a request never grows
	// them. Every ledger row's member list is sized for the largest batch, a job serving every
	// sequence block, so filling a row never allocates either. The tokens-per-second window is
	// sized here for the same reason.
	S.JobLedger.Init(ReportHistoryRows());
	for (FSuperSLMWorkerJobReport& Row : S.JobLedger.Slots())
	{
		Row.MemberSequences.Reserve(Config.BlockCount);
	}
	S.TickHistory.Init(ReportHistoryRows());
	S.TokensPerSecondRing.Reserve(FSuperSLMSubsystemState::TokensPerSecondWindowCapacity + 1);

	S.PublishStats(FSuperSLMTickReport());
	Report.Result = ESuperSLMConfigureResult::Success;
	if (bBudgetMode)
	{
		using SuperSLMPaging::ERegion;
		Report.Message = FString::Printf(
			TEXT("budget mode (SequenceBudget %d): KV page pool %lld bytes (%u pages of %llu bytes, B = %lld: S %u for %d sequences, P %u, H %u; + %llu overhead), %d worker lane(s) with a %lld-byte workspace and a %lld-byte save buffer each, token finish %s; predicted reset %.2f ms, adopt %.2f ms"),
			Config.SequenceBudget, S.KvBytes, PoolPages, static_cast<uint64>(PageBytes), S.PagePositions,
			S.Ledger.RegionCapacity(ERegion::S), Config.BlockCount, S.Ledger.RegionCapacity(ERegion::P), S.Ledger.RegionCapacity(ERegion::H),
			static_cast<uint64>(PageOverheadBytes), S.Lanes.Num(), S.WorkspaceBytesEach, S.SaveStagingBytes,
			SuperSLMFinishHook::ShouldInstall(Resolved.ParallelTasks) ? *FString::Printf(TEXT("split into at most %d ParallelFor tasks"), Resolved.ParallelTasks) : TEXT("serial"),
			Lifecycle.PredictedResetMs, Lifecycle.PredictedAdoptMs);
		OutState = NewState;
		return Report;
	}
	Report.Message = FString::Printf(
		TEXT("KV pool %lld bytes (%d sequence + %d prefix blocks of %llu bytes, + %llu overhead), %d worker lane(s) with a %lld-byte workspace and a %lld-byte save buffer each, token finish %s; predicted reset %.2f ms, adopt %.2f ms"),
		S.KvBytes, Config.BlockCount, Config.PrefixBlockCount, static_cast<uint64>(BlockBytes), static_cast<uint64>(OverheadBytes),
		S.Lanes.Num(), S.WorkspaceBytesEach, S.SaveStagingBytes,
		SuperSLMFinishHook::ShouldInstall(Resolved.ParallelTasks) ? *FString::Printf(TEXT("split into at most %d ParallelFor tasks"), Resolved.ParallelTasks) : TEXT("serial"),
		Lifecycle.PredictedResetMs, Lifecycle.PredictedAdoptMs);
	OutState = NewState;
	return Report;
}
