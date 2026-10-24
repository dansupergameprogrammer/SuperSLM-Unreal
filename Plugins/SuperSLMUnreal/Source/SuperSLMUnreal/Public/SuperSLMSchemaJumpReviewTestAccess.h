#pragma once

// Test-only access for the cells schema fast-decode plan rev 1.3.6 added after the P1/P1-X code review
// (round 1; §17.12): P1.10 (the walk table's load), P1.11 (the scheduler rules) and P1X.12 (f) (a
// stop token inside a confirmed draft). Compiled only under WITH_DEV_AUTOMATION_TESTS; nothing here
// changes a token, a price or a schedule. Game thread only.
//
// Built: the bodies live in Private/SuperSLMSubsystem.cpp, beside the state they read (as the first
// seam's do), and IsBuilt() reads true there. The stub file is gone.

#include "CoreMinimal.h"
#include "SuperSLMSequenceTypes.h"

class USuperSLMSubsystem;

#if WITH_DEV_AUTOMATION_TESTS

// One load of the walk table (the SchemaMasks/SCM1 section the plugin walks, §6 P1-X "The walk").
struct SUPERSLMUNREAL_API FSuperSLMSchemaWalkLoadRecord
{
	// Whether any part of the load ran on the game thread (P1.10 (a), (c); mutant XD8).
	bool bAnyOnGameThread = true;
	// Whether any part of it ran inside Configure() or BeginConfigure()'s preparation.
	bool bInsideConfigure = true;
	// Bytes of the artifact the load hashed (SHA-256) and bytes it copied. The section's own bytes
	// count as copied when the load copies them; the artifact's bytes when it copies the artifact.
	int64 ArtifactBytesHashed = -1;
	int64 BytesCopied = -1;
	// The SchemaMasks section's size in the mapped artifact, and the artifact's size.
	int64 SectionBytes = -1;
	int64 ArtifactBytes = -1;
	// Whether SslmModel::Load (or any whole-model parse) ran for it.
	bool bWholeModelLoad = true;
	// Whether the table became ready (the walk known from then on), and the game-thread tick it did.
	bool bReady = false;
	int32 ReadyAtTick = -1;
};

// One forcedness read (sslm_seq_forced_tokens, cap 0) the subsystem made for a generation.
struct SUPERSLMUNREAL_API FSuperSLMForcednessReadRecord
{
	FSuperSLMSequence Sequence;
	// The JobId (the one counter of ledger rows and span jobs) of the job the read followed: a
	// prefill or decode row, or a span job.
	int64 AfterJobId = 0;
	// out_count as the engine reported it (0: no chain; -1 when the call failed).
	int32 ForcedCount = -1;
	// Whether the plugin recorded it against a walk state (only while the walk is known).
	bool bRecordedAtWalkState = false;
	uint32 WalkState = 0;
};

struct SUPERSLMUNREAL_API FSuperSLMSchemaJumpReviewTestAccess
{
	// True: the seam is built (its bodies are in Private/SuperSLMSubsystem.cpp); the cells read it first.
	static bool IsBuilt();

	// The walk-table loads since the configured subsystem's last Configure() or BeginConfigure()
	// began, that call's own preparation included, oldest first. Empty when none ran.
	static TArray<FSuperSLMSchemaWalkLoadRecord> GetSchemaWalkLoads(const USuperSLMSubsystem& Subsystem);

	// Whether the walk table is ready (loaded and parsed) on the configured subsystem.
	static bool IsSchemaWalkReady(const USuperSLMSubsystem& Subsystem);

	// Holds the next walk-table load before it builds anything until released (P1.10 (c): the walk
	// stays unknown while a load is in flight). A later Configure() clears the hold.
	static void HoldSchemaWalkLoad(USuperSLMSubsystem& Subsystem, bool bHold);

	// Added with the implementation (rev 1.3.6, the plan's ruling 4): loads the walk table now, as the
	// first jump-mode generation would, and waits for it; true once it is ready. A held load is not
	// waited for (false). Ruling 4 means Off generations make no forcedness reads, and so seed nothing,
	// until the table is loaded; a cell that seeds from Off runs made before any jump-mode generation
	// calls this after Configure(). The load it makes is one of GetSchemaWalkLoads()'s.
	static bool LoadSchemaWalkTableNow(USuperSLMSubsystem& Subsystem);

	// The forcedness reads made since FSuperSLMSchemaJumpTestAccess::StartRecording(), in the order
	// they were made (P1.11 (b), (e), (f)).
	static TArray<FSuperSLMForcednessReadRecord> GetForcednessReads(const USuperSLMSubsystem& Subsystem);

	// Ground truth for the drain rule (P1.2, P1.11 (c); review finding 4): under recording, before
	// each decode job is planned for a Canonical schema-bound sequence, the seam itself reads
	// sslm_seq_forced_tokens (cap 0) and counts the decode jobs planned while it reported a chain
	// (out_count > 0). The seam's own read is not one of GetForcednessReads()'s.
	static int32 GetCanonicalDecodesPlannedWithOpenChain(const USuperSLMSubsystem& Subsystem);

	// Whether the walk is known for Sequence's current generation (§6 P1 "When the walk is known").
	static bool IsWalkKnown(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence);

	// The vended sequence's context length as the subsystem holds it (its ContextUsed mirror; -1 for a handle that
	// is not vended), and the configured context cap (-1 when not configured). The nested cache adopt cells read
	// both.
	static int64 GetContextUsed(const USuperSLMSubsystem& Subsystem, const FSuperSLMSequence& Sequence);
	static int64 GetContextCap(const USuperSLMSubsystem& Subsystem);
};

#endif // WITH_DEV_AUTOMATION_TESTS
