"""T-2226 -- L2-S0 red suite. Fixture integrity, added in the T-2226 fix round routed
by the implementation (the build record §8): `l2s0_overlapping_sections.
sslm`'s original generator set row 5's offset to `row4_offset + row4_size - 64` without
checking that value against ANY of the loader's own prior checks. For this fixture's real
geometry (row4_size = 30380, not a multiple of 64) that arithmetic landed on 118316, and
118316 % 64 == 44 -- Layer 1's per-section alignment check (`git -C <SuperSLM checkout> show
v1.5.0:src/artifact.cpp`, line 369, `Misaligned`) runs in table order BEFORE either
`SectionOverlap` site (390, 423), so it fired first and the fixture never reached the check
it claims to trip. The plugin under test reported Layer 1's TRUE status (`Misaligned`) for the
bytes it was actually handed -- the defect was the fixture not constructing the condition it
claimed to, not the plugin's diagnostic. (Line numbers re-derived at the T-2788 re-pin,
2026-09-18, from v1.2.0's ~334/~355/~388 -- the check order is unchanged, only the source
moved.)

This file makes that class of defect structural rather than a thing caught only by an
authoring-time hand check (a rule enforced by memory is vigilance, and vigilance fails): every committed hostile `.sslm` fixture is checked, on every run,
against an independent re-implementation of the loader's own validation order (derived from
`git show v1.5.0:src/artifact.cpp` lines 248-344 and the per-section loop immediately after,
never from the plugin's own code, which does not exist yet), and MUST be caught by exactly the
check its filename/PROVENANCE.md entry claims -- not an earlier one. A regenerated fixture
that regresses to tripping the wrong check fails HERE, at authoring/regeneration time, rather
than surfacing three build rounds later as a plugin diagnostic mismatch.
"""
import hashlib
import struct
from pathlib import Path

FIXTURES_DIR = (
    Path(__file__).resolve().parents[2]
    / "Source" / "SuperSLMUnrealEditor" / "Private" / "Tests" / "Fixtures" / "SSLM"
)

_HEADER_BYTES = 64
_SECTION_DESC_BYTES = 40
_INTEGRITY_HASH_OFFSET = 32
_INTEGRITY_HASH_BYTES = 32
_KNOWN_FLAGS_MASK = 0x7  # v1.5.0: kOptionGFusedKLandingFlag|kDampedGreedyArtifactConstantsFlag|kQkNormFusedKChannelTableFlag


def _first_failing_check(data: bytes) -> str:
    """Independent re-implementation of superslm::SslmArtifact::OpenFromMemory's check
    order, grounded at `git show v1.5.0:src/artifact.cpp` (see module docstring) -- derived
    from the source's own control flow, never from this plugin's code (which does not exist).
    Returns the name of the first check that would reject `data`, or "OK"."""
    size = len(data)
    if size < _HEADER_BYTES:
        return "Truncated(header)"
    if data[0:4] != b"SSLM":
        return "BadMagic"
    version = struct.unpack_from("<I", data, 4)[0]
    if version != 2:
        return "UnsupportedVersion"
    header_bytes = struct.unpack_from("<I", data, 8)[0]
    if header_bytes != _HEADER_BYTES:
        return "BadHeader(header_bytes)"
    section_count = struct.unpack_from("<I", data, 12)[0]
    flags = struct.unpack_from("<I", data, 16)[0]
    reserved0 = struct.unpack_from("<I", data, 20)[0]
    if (flags & ~_KNOWN_FLAGS_MASK) != 0:
        return "BadHeader(flags)"
    if reserved0 != 0:
        return "BadHeader(reserved0)"
    if section_count > 4096:
        return "TooManySections"
    table_end = _HEADER_BYTES + section_count * _SECTION_DESC_BYTES
    if table_end > size:
        return "Truncated(table)"
    file_bytes = struct.unpack_from("<Q", data, 24)[0]
    if file_bytes != size:
        return "FileSizeMismatch"

    tmp = bytearray(data)
    tmp[_INTEGRITY_HASH_OFFSET:_INTEGRITY_HASH_OFFSET + _INTEGRITY_HASH_BYTES] = (
        b"\x00" * _INTEGRITY_HASH_BYTES
    )
    digest = hashlib.sha256(bytes(tmp)).digest()
    if digest != data[_INTEGRITY_HASH_OFFSET:_INTEGRITY_HASH_OFFSET + _INTEGRITY_HASH_BYTES]:
        return "IntegrityMismatch"

    # Per-section checks in TABLE ORDER: bounds and alignment are checked per row before the
    # pairwise overlap sweep that follows -- this ordering is exactly what let the routed
    # defect happen (a misalignment on row 5 fired before the overlap sweep ever ran).
    ranges = []
    for i in range(section_count):
        row = _HEADER_BYTES + i * _SECTION_DESC_BYTES
        off, sz = struct.unpack_from("<QQ", data, row + 8)
        align = struct.unpack_from("<I", data, row + 32)[0]
        if align != 0 and off % align != 0:
            return f"Misaligned(row {i})"
        if off + sz > file_bytes:
            return f"OutOfBounds(row {i})"
        ranges.append((off, off + sz, i))
    for a, b in zip(sorted(ranges), sorted(ranges)[1:]):
        if a[1] > b[0]:
            return f"SectionOverlap(rows {a[2]},{b[2]})"
    return "OK"


# (filename, expected first-failing check) -- matches PROVENANCE.md's own table.
_EXPECTED = [
    ("l2s0_valid_reference.sslm", "OK"),
    ("l2s0_bad_magic.sslm", "BadMagic"),
    ("l2s0_unknown_flag_bit.sslm", "BadHeader(flags)"),
    ("l2s0_truncated.sslm", "FileSizeMismatch"),
    ("l2s0_hash_mismatch.sslm", "IntegrityMismatch"),
    ("l2s0_overlapping_sections.sslm", "SectionOverlap(rows 4,5)"),
]


def test_every_fixture_trips_exactly_its_claimed_check():
    results = {}
    for filename, expected in _EXPECTED:
        path = FIXTURES_DIR / filename
        assert path.is_file(), f"fixture missing: {path}"
        data = path.read_bytes()
        results[filename] = _first_failing_check(data)
    mismatches = {
        name: (results[name], expected)
        for name, expected in _EXPECTED
        if results[name] != expected
    }
    assert not mismatches, (
        "one or more committed fixtures no longer trip the check their filename/PROVENANCE.md "
        f"claims -- got vs. expected: {mismatches!r}"
    )


def test_overlapping_sections_row5_offset_satisfies_all_four_properties():
    """The specific regression this file exists to close: row 5's planted offset in
    `l2s0_overlapping_sections.sslm` must satisfy FOUR properties simultaneously, or the
    fixture trips the wrong check (as the original generator did -- see module docstring).
    Asserted directly against the committed bytes, not re-derived from the generator, so a
    hand-edit to either file that breaks the other is caught here."""
    path = FIXTURES_DIR / "l2s0_overlapping_sections.sslm"
    data = path.read_bytes()

    section_count = struct.unpack_from("<I", data, 12)[0]
    file_bytes = struct.unpack_from("<Q", data, 24)[0]
    table_end = _HEADER_BYTES + section_count * _SECTION_DESC_BYTES

    row4_off, row4_size = struct.unpack_from("<QQ", data, _HEADER_BYTES + 4 * _SECTION_DESC_BYTES + 8)
    row5_off, row5_size = struct.unpack_from("<QQ", data, _HEADER_BYTES + 5 * _SECTION_DESC_BYTES + 8)
    row5_align = struct.unpack_from("<I", data, _HEADER_BYTES + 5 * _SECTION_DESC_BYTES + 32)[0]
    row4_end = row4_off + row4_size

    # (1) In-file: row 5's mutated range does not run past the actual file length.
    assert row5_off + row5_size <= file_bytes, (
        f"row5 range [{row5_off}, {row5_off + row5_size}) exceeds file_bytes {file_bytes}"
    )
    # (2) Past the header/section-table region.
    assert row5_off >= table_end, (
        f"row5 offset {row5_off} must be >= table_end {table_end}"
    )
    # (3) 64-aligned, matching row5's own declared alignment field -- this is the exact
    #     property the original generator violated (offset 118316, 118316 % 64 == 44).
    assert row5_off % row5_align == 0, (
        f"row5 offset {row5_off} is not a multiple of its own declared alignment {row5_align}"
    )
    # (4) Genuinely inside row 4's byte range -- an adjacent-but-non-overlapping offset would
    #     satisfy (1)-(3) while still failing to trip SectionOverlap at all.
    assert row4_off < row5_off < row4_end, (
        f"row5 offset {row5_off} must land strictly inside row4's range "
        f"[{row4_off}, {row4_end}) to be a genuine overlap"
    )


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            try:
                fn()
                print(f"{name}: PASS")
            except AssertionError as e:
                print(f"{name}: FAIL -- {e}")
