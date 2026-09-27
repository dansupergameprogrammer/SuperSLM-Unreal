#pragma once

// T-2227 fix round 5 (D-SLM3959, §9 dim 9(d)). Test-only allocator-hook utility --
// SuperSLMPackagedMemoryMappingTests.cpp's own header specifies this interface (not read from
// any implementation-owned file); this header supplies it. NOT production code: it lives under the
// DeveloperTool test module's Private/Tests/, matching SuperSLMImportFixtures.h's own
// "test-only, never consumed by a runtime or editor module path" convention.
//
// T-2447 (D-SLM5340): moved here from SuperSLMUnrealEditor. The cells that measure through
// this utility are ClientContext cells a packaged TargetType.Game executable must run, and an
// "Type": "Editor" module is never loaded by one.
//
// A scoped FMalloc proxy: while an FSuperSLMAllocationScope is alive, every Malloc/Realloc
// call the CURRENT GMalloc receives is also seen here, so HasAllocationAtLeast(MinBytes) can
// answer "did anything request at least this many bytes in one call while I was watching" --
// the oracle §9 dim 9(d)'s memory-mapping cells need to prove a mapped load performs NO
// artifact-sized heap allocation, and the one L2-S1's own dim 7 hot-path cell will need for
// the identical reason ("no allocation this large" is the same question at both places).
//
// T-2256 fix round 11 (D-SLM4006, §9 dim 9(e)): PeakNetAllocatedBytes() -- a high-water mark
// of (bytes Malloc/Realloc'd minus bytes Free'd) observed while the scope was live, over
// pointers THIS scope saw allocated. Distinct from HasAllocationAtLeast()'s "largest single
// call" oracle: a real-scale mapped load's non-payload bookkeeping may individually be small
// while summing to a delta worth bounding. Sizes are tracked per live pointer (not queried
// from the inner allocator on Free) deliberately -- subtracting sizes for blocks allocated
// BEFORE the scope went live would credit their frees against this scope's traffic and drive
// the observed peak DOWN, which is the one direction of error that weakens the oracle.
//
// ---------------------------------------------------------------------------------------
// T-2447 (D-SLM5345) -- the two defects that had this instrument's readings quarantined, and
// what each fix is.
//
// (1) LIFETIME, not a barrier. The proxy used to BE the object the test declares on its
//     RunTest stack frame, installed as the process-wide UE::Private::GMalloc for the length
//     of that frame and restored on the way out. GMalloc serves EVERY thread in the process,
//     so a thread already dispatched into Malloc when the frame returned went on calling a
//     destroyed object. No drain protocol closes that window: a thread has already READ the
//     GMalloc pointer before it reaches any counter a teardown could wait on, so there is no
//     point at which "nobody can still enter" becomes true. The fix is therefore to remove
//     the destruction rather than to guard it. The proxy is a process-lifetime singleton
//     that is NEVER destroyed -- which is exactly what the engine's own FMalloc proxies do --
//     and FSuperSLMAllocationScope is now a stack ARM/DISARM handle over it. A late caller
//     lands on a live object and is forwarded to the real allocator; the only residual is one
//     small allocation that outlives the process's last test, deliberately.
//
//     The proxy's inner-allocator pointer is atomic for the same reason: arming re-reads the
//     then-current GMalloc, and a concurrent forwarder must see either the old or the new
//     value and never a torn one.
//
// (2) ORACLE CONTAMINATION. RecordRequestedSize() -- the largest-single-allocation oracle
//     HasAllocationAtLeast() answers from -- was called from Malloc and Realloc AHEAD of the
//     IsRecording() reentrancy guard, which protected only RecordAllocation. This
//     instrument's own TMap growth allocates, re-enters through the proxy, and so raised the
//     observed maximum with the instrument's own bookkeeping block sizes -- contaminating the
//     exact number MM-1 decided on. The guard now lives INSIDE RecordRequestedSize, so it is
//     a property of the recorder rather than a rule each call site has to remember
//     (where a rule can be made structural, make it structural: a rule kept by memory fails).
// ---------------------------------------------------------------------------------------

#include "CoreMinimal.h"
#include "Containers/Map.h"
#include "HAL/CriticalSection.h"
#include "HAL/UnrealMemory.h"
#include "Misc/ScopeLock.h"
#include <atomic>

// The FMalloc proxy itself. Constructed once per process and DELIBERATELY NEVER DESTROYED
// (see (1) in the header comment): every observation window is an Arm()/Disarm() pair on this
// one long-lived object rather than a fresh object installed and torn down.
class FSuperSLMAllocationScopeProxy final : public FMalloc
{
public:
	explicit FSuperSLMAllocationScopeProxy(FMalloc* InInner)
		: Inner(InInner)
	{
	}

	FSuperSLMAllocationScopeProxy(const FSuperSLMAllocationScopeProxy&) = delete;
	FSuperSLMAllocationScopeProxy& operator=(const FSuperSLMAllocationScopeProxy&) = delete;

	// Open an observation window: adopt the currently-installed allocator as the inner one,
	// clear every counter and the live-pointer map, then begin recording. The caller installs
	// this proxy as UE::Private::GMalloc immediately afterwards -- recording is enabled first
	// so no allocation can arrive through the proxy before the reset that would erase it.
	void Arm(FMalloc* InInner)
	{
		checkf(!bArmed.load(std::memory_order_relaxed),
			TEXT("FSuperSLMAllocationScope does not nest -- a second scope was opened while "
				"one was already live, which would silently share one set of counters"));
		Inner.store(InInner, std::memory_order_relaxed);
		{
			FScopeLock Lock(&LiveSizesCriticalSection);
			LiveSizes.Reset();
		}
		MaxObservedSingleAllocationBytes.store(0, std::memory_order_relaxed);
		NetAllocatedBytes.store(0, std::memory_order_relaxed);
		PeakNetAllocatedBytesValue.store(0, std::memory_order_relaxed);
		bArmed.store(true, std::memory_order_relaxed);
	}

	// Close the observation window. Called after the proxy has been uninstalled from
	// UE::Private::GMalloc; calls still arriving from a thread that read the old pointer keep
	// landing on this LIVE object and are forwarded to Inner, now unrecorded. The map's
	// storage is released here rather than left to accumulate across scopes.
	void Disarm()
	{
		bArmed.store(false, std::memory_order_relaxed);
		FScopeLock Lock(&LiveSizesCriticalSection);
		LiveSizes.Empty();
	}

	FMalloc* GetInner() const
	{
		return Inner.load(std::memory_order_relaxed);
	}

	// True if any single Malloc/Realloc call observed while this scope was live requested at
	// least MinBytes. A cooked-Windows mapped load is asserted to observe none such at the
	// artifact's own byte count -- a mapped view is not a heap copy.
	bool HasAllocationAtLeast(SIZE_T MinBytes) const
	{
		return static_cast<SIZE_T>(MaxObservedSingleAllocationBytes.load(std::memory_order_relaxed)) >= MinBytes;
	}

	// High-water mark of (bytes Malloc/Realloc'd minus bytes Free'd) across calls observed
	// while this scope was live, counted over pointers this scope itself saw allocated.
	// Distinct from HasAllocationAtLeast()'s "largest single call" oracle above: a real-scale
	// mapped load's non-payload bookkeeping may individually be small while summing to a
	// delta worth bounding. Frees of blocks allocated BEFORE the scope went live are passed
	// through without subtracting -- crediting those frees against this scope would drive the
	// observed peak DOWN, the one direction of error that weakens an upper-bound oracle, so
	// this peak only ever reflects traffic this scope actually saw.
	int64 PeakNetAllocatedBytes() const
	{
		return PeakNetAllocatedBytesValue.load(std::memory_order_relaxed);
	}

	//~ Begin FMalloc interface -- pure pass-through to the wrapped allocator, recording the
	//~ largest single requested size seen, and maintaining the per-live-pointer size map the
	//~ net high-water mark needs. Forwarding is unconditional: it does not depend on this
	//~ proxy being armed, or even still installed, which is what makes a late caller safe.
	virtual void* Malloc(SIZE_T Size, uint32 Alignment = DEFAULT_ALIGNMENT) override
	{
		void* Result = GetInner()->Malloc(Size, Alignment);
		if (Result != nullptr)
		{
			RecordRequestedSize(Size);
			RecordAllocation(Result, Size);
		}
		return Result;
	}

	virtual void* Realloc(void* Original, SIZE_T Size, uint32 Alignment = DEFAULT_ALIGNMENT) override
	{
		if (Original == nullptr)
		{
			return Malloc(Size, Alignment);
		}

		void* Result = GetInner()->Realloc(Original, Size, Alignment);
		if (Result == nullptr && Size != 0)
		{
			// Contractually near-unreachable (UE allocators abort on OOM rather than return
			// null), but if it ever happens the original block is still valid and must stay
			// recorded exactly as it was.
			return nullptr;
		}

		// Subtract the old block only if THIS scope recorded it -- a pre-scope block grown
		// in place contributes its new requested size from now on, which is the conservative
		// upward direction of error for an upper-bound oracle.
		const int64 ForgottenBytes = ForgetAllocation(Original);
		if (ForgottenBytes > 0)
		{
			AdjustNetAndRaisePeak(-ForgottenBytes);
		}
		if (Size > 0)
		{
			RecordRequestedSize(Size);
			RecordAllocation(Result, Size);
		}
		return Result;
	}

	virtual void Free(void* Original) override
	{
		if (Original != nullptr)
		{
			const int64 ForgottenBytes = ForgetAllocation(Original);
			if (ForgottenBytes > 0)
			{
				AdjustNetAndRaisePeak(-ForgottenBytes);
			}
		}
		GetInner()->Free(Original);
	}

	virtual const TCHAR* GetDescriptiveName() override
	{
		return TEXT("FSuperSLMAllocationScope");
	}
	//~ End FMalloc interface

private:
	// Record a freshly-returned live block of the given requested size: add it to the
	// per-live-pointer map (so a later Free/Realloc of exactly this pointer can subtract its
	// bytes) and raise the net by Size. The map's own internal allocations arrive back
	// through this proxy (TMap grows via FMemory::Malloc); the thread-local guard makes those
	// reentrant calls pass straight through unrecorded, so instrument overhead is never
	// attributed to the measured load.
	void RecordAllocation(void* Ptr, SIZE_T Size)
	{
		if (Size == 0 || !bArmed.load(std::memory_order_relaxed) || IsRecording())
		{
			return;
		}
		EnterRecording();
		{
			FScopeLock Lock(&LiveSizesCriticalSection);
			LiveSizes.Add(Ptr, static_cast<SIZE_T>(Size));
		}
		ExitRecording();
		AdjustNetAndRaisePeak(static_cast<int64>(Size));
	}

	// Remove a pointer's recorded size, answering the bytes it contributed (0 if this scope
	// never recorded it -- pre-scope blocks are passed through without accounting).
	int64 ForgetAllocation(void* Ptr)
	{
		if (!bArmed.load(std::memory_order_relaxed) || IsRecording())
		{
			return 0;
		}
		SIZE_T RecordedSize = 0;
		bool bFound = false;
		EnterRecording();
		{
			FScopeLock Lock(&LiveSizesCriticalSection);
			bFound = LiveSizes.RemoveAndCopyValue(Ptr, RecordedSize);
		}
		ExitRecording();
		return bFound ? static_cast<int64>(RecordedSize) : 0;
	}

	void AdjustNetAndRaisePeak(int64 Delta)
	{
		const int64 NewNet = NetAllocatedBytes.fetch_add(Delta, std::memory_order_relaxed) + Delta;
		int64 ObservedPeak = PeakNetAllocatedBytesValue.load(std::memory_order_relaxed);
		while (NewNet > ObservedPeak &&
			!PeakNetAllocatedBytesValue.compare_exchange_weak(ObservedPeak, NewNet, std::memory_order_relaxed))
		{
		}
	}

	// The HasAllocationAtLeast() oracle: the largest single requested size seen so far.
	//
	// D-SLM5345 defect (2): the arm/reentrancy test lives HERE rather than at the two call
	// sites in Malloc and Realloc. Without it this instrument's own TMap growth re-enters
	// through the proxy and raises the observed maximum with the instrument's own bookkeeping
	// block sizes, contaminating the number MM-1 decided on. Placing the guard inside the
	// recorder makes it impossible for a call site to omit.
	void RecordRequestedSize(SIZE_T Size)
	{
		if (!bArmed.load(std::memory_order_relaxed) || IsRecording())
		{
			return;
		}
		int64 SizeAsInt64 = static_cast<int64>(Size);
		int64 Previous = MaxObservedSingleAllocationBytes.load(std::memory_order_relaxed);
		while (SizeAsInt64 > Previous &&
			!MaxObservedSingleAllocationBytes.compare_exchange_weak(Previous, SizeAsInt64, std::memory_order_relaxed))
		{
		}
	}

	// Reentrancy guard for the recording structures themselves. GMalloc serves every thread;
	// TMap growth allocates; without this guard a map resize inside RecordAllocation would
	// recurse into this proxy and, worse, count the instrument against the measurement.
	bool IsRecording() const
	{
		return RecordingDepth.load(std::memory_order_relaxed) > 0;
	}

	void EnterRecording()
	{
		RecordingDepth.store(RecordingDepth.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	}

	void ExitRecording()
	{
		RecordingDepth.store(RecordingDepth.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
	}

	std::atomic<FMalloc*> Inner;
	std::atomic<bool> bArmed{false};
	std::atomic<int64> MaxObservedSingleAllocationBytes{0};
	std::atomic<int64> NetAllocatedBytes{0};
	std::atomic<int64> PeakNetAllocatedBytesValue{0};

	// Per-thread depth rather than a single bool: a nested instrumented call on the SAME
	// thread must pass through unrecorded, while other threads record normally. Inline so
	// this header-only utility needs no companion .cpp.
	inline static thread_local std::atomic<int32> RecordingDepth{0};

	FCriticalSection LiveSizesCriticalSection;
	TMap<void*, SIZE_T> LiveSizes;
};

// The one proxy this process ever has. Allocated on first use through the then-current
// allocator and never freed: its whole purpose is to outlive every thread that might still
// hold the GMalloc pointer it was installed under (D-SLM5345 defect (1)).
inline FSuperSLMAllocationScopeProxy& SuperSLMAllocationScopeProxy()
{
	// GMalloc must already exist by the time any test can run (the engine's own startup
	// allocator is live long before automation tests execute) -- this proxy only ever wraps a
	// real allocator, never bootstraps one.
	check(UE::Private::GMalloc != nullptr);
	static FSuperSLMAllocationScopeProxy* Proxy = new FSuperSLMAllocationScopeProxy(UE::Private::GMalloc);
	return *Proxy;
}

// The stack handle the cells declare. Its lifetime is one observation window; the object it
// records through outlives it.
class FSuperSLMAllocationScope
{
public:
	FSuperSLMAllocationScope()
		: Proxy(SuperSLMAllocationScopeProxy())
	{
		// Writes through UE::Private::GMalloc (MemoryBase.h) rather than the public `GMalloc`
		// alias, which UE 5.6+ made a deprecated const reference specifically to steer
		// ordinary callers to FMemory::Malloc -- swapping the ACTIVE allocator (what this
		// scope does, the same thing every built-in malloc proxy in UnrealMemory.cpp does) is
		// the one legitimate remaining use of the real, mutable global.
		FMalloc* CurrentAllocator = UE::Private::GMalloc;
		checkf(CurrentAllocator != &Proxy,
			TEXT("the allocation-scope proxy is already installed as GMalloc -- adopting it as "
				"its own inner allocator would recurse forever"));
		Proxy.Arm(CurrentAllocator);
		UE::Private::GMalloc = &Proxy;
	}

	~FSuperSLMAllocationScope()
	{
		// Only restore if nothing nested further wrapped GMalloc after this scope did --
		// scopes of this type are not designed to nest, and a mismatched restore would drop
		// whichever proxy was installed on top. Either way the proxy stays alive, so a thread
		// still dispatched inside it is forwarding through a live object rather than a
		// destroyed one.
		if (UE::Private::GMalloc == &Proxy)
		{
			UE::Private::GMalloc = Proxy.GetInner();
		}
		Proxy.Disarm();
	}

	FSuperSLMAllocationScope(const FSuperSLMAllocationScope&) = delete;
	FSuperSLMAllocationScope& operator=(const FSuperSLMAllocationScope&) = delete;

	bool HasAllocationAtLeast(SIZE_T MinBytes) const
	{
		return Proxy.HasAllocationAtLeast(MinBytes);
	}

	int64 PeakNetAllocatedBytes() const
	{
		return Proxy.PeakNetAllocatedBytes();
	}

private:
	FSuperSLMAllocationScopeProxy& Proxy;
};
