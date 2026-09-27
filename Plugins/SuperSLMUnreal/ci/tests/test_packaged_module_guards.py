"""T-2448 -- pin for the T-2447 guard that cannot be executed from any pass this suite can
run.

The guard lives inside `SuperSLMUnrealTests`, and refuses an input rather than computing a
value, which is why it is not pinned by an automation cell here:

- **The instrument's self-adoption guard.** `FSuperSLMAllocationScope`'s constructor carries
  `checkf(CurrentAllocator != &Proxy, ...)`, refusing the one input that would install the
  proxy as its own inner allocator and recurse forever. A `checkf` is fatal: it terminates the
  process rather than failing a cell, so no automation test can observe it firing. What an
  automation cell CAN establish is that the refused state is genuinely producible by the real
  path -- that is
  `FSuperSLMAllocationScopeSelfAdoptionRefusedStateIsReachableTest`
  (`SuperSLMAllocationScopeInstrumentTests.cpp`), which asserts the proxy IS the installed
  `GMalloc` while a scope is live, so a second construction would arrive at exactly the
  comparison this guard makes. This file is the other half: that the constructor still MAKES
  that comparison, and still makes it BEFORE arming.
  Mutation: remove the `checkf` from the handle's constructor.

MM-1's strict-positivity gate pin (tests f-h below, in the prior revision of this file) is
REMOVED at T-2788 U0 (plan §10.1 item 6, D-SLM7221): MM-1 itself was removed (fold record §4.1
dim 9(d), D-SLM7220) -- none had ever executed. The strict-positivity gate itself did not go
with it: MM-5 (`SuperSLMPackagedMemoryMappingTests.cpp`'s
`FSuperSLMMemoryMappingRealScaleArtifactTest`) carries its own strict-positivity gate on
`ArtifactByteCount`. Nothing pins that gate, BY DECISION (D-SLM7305): it matters only at the
single, opt-in L2-S4 run against a real artifact, and that run's `AddInfo` reports the byte
count regardless of whether the gate exists.

Every cell drives a pure predicate over file text, and every must-reject drives the SAME
predicate against a scratch copy of the real file with the mutation planted -- so the
discrimination proof is re-executed on every run (the commissioning
rule; `test_thirdparty_vendoring.py`'s `test_d`/`test_d2` shape).
"""
import re
from pathlib import Path

PLUGIN_ROOT = Path(__file__).resolve().parents[2]
TESTS_MODULE_ROOT = PLUGIN_ROOT / "Source" / "SuperSLMUnrealTests"
ALLOCATION_SCOPE_H = TESTS_MODULE_ROOT / "Private" / "Tests" / "SuperSLMAllocationScope.h"


def _braced_body_after(text: str, signature: str) -> str:
    """The brace-matched block that follows `signature`'s first occurrence.

    Fails loudly (ValueError) if the signature is absent, so a rename that moves the guard out
    from under this checker's feet reds the cell rather than silently scanning nothing.
    """
    start = text.index(signature)
    open_brace = text.index("{", start)
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace : i + 1]
    raise ValueError(f"unbalanced braces after {signature!r}")


# --------------------------------------------------------------------------------------- #
# The instrument's self-adoption guard
# --------------------------------------------------------------------------------------- #

_SELF_ADOPTION_GUARD = re.compile(r"checkf\(\s*[^,]*!=\s*&\s*Proxy\s*,")
_ARM_CALL = re.compile(r"Proxy\s*\.\s*Arm\s*\(")


def _self_adoption_guard_state(header_text: str) -> dict[str, object]:
    """{present, precedes_arm} for the handle constructor's self-adoption guard."""
    body = _braced_body_after(header_text, "FSuperSLMAllocationScope()")
    guard = _SELF_ADOPTION_GUARD.search(body)
    arm = _ARM_CALL.search(body)
    return {
        "present": guard is not None,
        "arm_present": arm is not None,
        "precedes_arm": bool(guard and arm and guard.start() < arm.start()),
    }


_NON_NESTING_GUARD = re.compile(r"checkf\(\s*!\s*bArmed")


def _non_nesting_guard_present(header_text: str) -> bool:
    """The proxy's own `Arm()` refusal of a second, overlapping observation window."""
    body = _braced_body_after(header_text, "void Arm(FMalloc* InInner)")
    return _NON_NESTING_GUARD.search(body) is not None


def test_a_handle_constructor_refuses_self_adoption_before_arming():
    """Must-accept. The guard exists and runs before `Arm()`: ordering is load-bearing, because
    `Arm()` stores the passed allocator as the proxy's inner one, so a guard placed after it has
    already let the recursive configuration be installed.
    """
    assert ALLOCATION_SCOPE_H.is_file(), f"{ALLOCATION_SCOPE_H} not found"
    state = _self_adoption_guard_state(ALLOCATION_SCOPE_H.read_text(encoding="utf-8"))
    assert state["present"], (
        "FSuperSLMAllocationScope's constructor carries no self-adoption guard -- nothing "
        "refuses installing the proxy as its own inner allocator, which forwards to itself "
        "forever on the next allocation from any thread in the process"
    )
    assert state["arm_present"], (
        "the constructor no longer calls Proxy.Arm() -- this checker's ordering assertion "
        "below has nothing to order against; re-read the constructor before trusting it"
    )
    assert state["precedes_arm"], (
        "the self-adoption guard runs AFTER Proxy.Arm() -- Arm() has already stored the proxy "
        "as its own inner allocator by then, so the guard fires on a configuration that is "
        "already installed"
    )


def test_b_must_reject_removing_the_self_adoption_guard_is_caught():
    """Must-reject for test_a, with its stated mutation: remove the `checkf` from the handle's
    constructor. Planted into a scratch copy of the real header.
    """
    real_text = ALLOCATION_SCOPE_H.read_text(encoding="utf-8")
    guard_statement = (
        "\t\tcheckf(CurrentAllocator != &Proxy,\n"
        "\t\t\tTEXT(\"the allocation-scope proxy is already installed as GMalloc -- adopting it as \"\n"
        "\t\t\t\t\"its own inner allocator would recurse forever\"));\n"
    )
    assert guard_statement in real_text, (
        f"mutation anchor not found in {ALLOCATION_SCOPE_H} -- the guard's exact text has "
        "changed; update the anchor before trusting this test's result"
    )
    mutated_text = real_text.replace(guard_statement, "", 1)
    assert mutated_text != real_text, "mutation did not apply"

    mutated_state = _self_adoption_guard_state(mutated_text)
    assert not mutated_state["present"], (
        "removed the self-adoption checkf from a scratch copy of the real header and the "
        "checker still reported it present -- the checker cannot discriminate"
    )


def test_c_must_reject_moving_the_guard_after_arm_is_caught():
    """Must-reject for test_a's ordering half: the same guard, moved to after `Proxy.Arm()`.
    A guard that is present but late passes a presence-only check, which is why the ordering
    assertion needs its own construction.
    """
    real_text = ALLOCATION_SCOPE_H.read_text(encoding="utf-8")
    guard_statement = (
        "\t\tcheckf(CurrentAllocator != &Proxy,\n"
        "\t\t\tTEXT(\"the allocation-scope proxy is already installed as GMalloc -- adopting it as \"\n"
        "\t\t\t\t\"its own inner allocator would recurse forever\"));\n"
    )
    arm_statement = "\t\tProxy.Arm(CurrentAllocator);\n"
    assert guard_statement in real_text and arm_statement in real_text, (
        f"mutation anchors not found in {ALLOCATION_SCOPE_H} -- update them before trusting "
        "this test's result"
    )
    mutated_text = real_text.replace(guard_statement, "", 1).replace(
        arm_statement, arm_statement + guard_statement, 1
    )
    assert mutated_text != real_text, "mutation did not apply"

    mutated_state = _self_adoption_guard_state(mutated_text)
    assert mutated_state["present"], (
        "the reordering construction lost the guard entirely -- it is then test_b's mutation, "
        "not this one, and the ordering assertion is not being exercised"
    )
    assert not mutated_state["precedes_arm"], (
        "moved the self-adoption guard after Proxy.Arm() in a scratch copy of the real header "
        "and the ordering checker did not catch it"
    )


def test_d_arm_refuses_a_second_overlapping_scope():
    """Must-accept for the non-nesting half of the same lifetime remedy. The proxy is a single
    process-lifetime object with ONE set of counters; a second scope opened while one is live
    would silently share them, so `Arm()` refuses rather than producing a reading nobody can
    attribute.
    """
    assert _non_nesting_guard_present(ALLOCATION_SCOPE_H.read_text(encoding="utf-8")), (
        "FSuperSLMAllocationScopeProxy::Arm() carries no checkf(!bArmed) -- a second, "
        "overlapping observation window would share one set of counters with the first and "
        "both readings would be unattributable"
    )


def test_e_must_reject_removing_the_non_nesting_guard_is_caught():
    """Must-reject for test_d: remove the `checkf(!bArmed ...)` from `Arm()`."""
    real_text = ALLOCATION_SCOPE_H.read_text(encoding="utf-8")
    guard_statement = (
        "\t\tcheckf(!bArmed.load(std::memory_order_relaxed),\n"
        "\t\t\tTEXT(\"FSuperSLMAllocationScope does not nest -- a second scope was opened while \"\n"
        "\t\t\t\t\"one was already live, which would silently share one set of counters\"));\n"
    )
    assert guard_statement in real_text, (
        f"mutation anchor not found in {ALLOCATION_SCOPE_H} -- the guard's exact text has "
        "changed; update the anchor before trusting this test's result"
    )
    mutated_text = real_text.replace(guard_statement, "", 1)
    assert len(real_text) - len(mutated_text) == len(guard_statement), (
        "the mutation removed something other than exactly the guard statement"
    )

    assert not _non_nesting_guard_present(mutated_text), (
        "removed the non-nesting checkf from a scratch copy of the real header and the "
        "checker still reported it present"
    )

