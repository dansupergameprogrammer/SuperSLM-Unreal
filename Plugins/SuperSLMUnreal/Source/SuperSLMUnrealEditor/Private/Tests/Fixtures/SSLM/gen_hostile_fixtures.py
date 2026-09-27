"""Spec-faithful hostile-.sslm mutator for T-2226 (SuperSLMUnreal L2-S0 red suite).

Derives every mutation from the field offsets in docs/sslm_format.md ("Byte
layout" -- header 64 bytes at offset 0, section table 40-byte rows at offset
64), independent of src/artifact.cpp's own implementation -- the same
discipline SuperSLM's own test suite uses for sslm_model_hostile_fixtures.h
(SuperSLM's tests/sslm_model_hostile_fixtures.h header comment).

Input: a structurally VALID .sslm artifact (this suite's l2s0_valid_reference.sslm,
generated 2026-08-22 via `git archive v1.2.0` of SuperSLM's
tools/_t2199_s8_synthetic_full_model_fixture.py -- a real synthetic model that
clears real engine construction in Layer 1's own T-2199 suite, not a
hand-typed placeholder). Every hostile fixture below is exactly ONE field
mutated from that valid base, matching every OTHER field including a
recomputed integrity hash where the loader's own check order (grounded at
`git show v1.5.0:src/artifact.cpp` lines 254-344, re-verified at the T-2788
re-pin: the check order and line numbers below moved but did not reorder)
requires the hash to still verify for the mutation's target check to be the
one that fires.
"""
import hashlib
import struct
import sys

kHeaderBytes = 64
kSectionDescBytes = 40
kIntegrityHashOffset = 32
kIntegrityHashBytes = 32
kKnownArtifactFlagsMask = 0x3  # kOptionGFusedKLandingFlag | kDampedGreedyArtifactConstantsFlag


def recompute_hash(data: bytearray) -> None:
    tmp = bytearray(data)
    tmp[kIntegrityHashOffset:kIntegrityHashOffset + kIntegrityHashBytes] = b"\x00" * kIntegrityHashBytes
    digest = hashlib.sha256(tmp).digest()
    data[kIntegrityHashOffset:kIntegrityHashOffset + kIntegrityHashBytes] = digest


def section_row_offsets(data: bytes):
    section_count = struct.unpack_from("<I", data, 12)[0]
    for i in range(section_count):
        row = kHeaderBytes + i * kSectionDescBytes
        yield i, row


def mk_bad_magic(base: bytes) -> bytes:
    # BadMagic -- checked at artifact.cpp:274 (v1.5.0), BEFORE the hash check
    # (lines 331-343), so no hash recompute is needed: the loader must never
    # reach the hash.
    data = bytearray(base)
    data[0] = ord('X')
    return bytes(data)


def mk_unknown_flag_bit(base: bytes) -> bytes:
    # BadHeader (unknown flags bit) -- artifact.cpp:299 (v1.5.0), also before
    # the hash check. Sets bit 0x80000000 (bit 31) -- docs/sslm_format.md's bit
    # allocation table (line 535 at v1.5.0) reserves this bit PERMANENTLY for
    # exactly this use: "Every 'the loader rejects an unknown flags bit'
    # fixture ... uses this bit and only this bit, so the claim under test can
    # never collide with a real allocation growing sequentially from bit 0."
    # T-2788 U0 first moved this fixture from bit 0x4 (became
    # kQkNormFusedKChannelTableFlag, a known bit, at v1.5.0) to bit 0x8, then
    # to 0x80000000 on the maintainer's ruling: 0x8 sits in the same
    # sequentially-allocated range (bits 3-30) a future real capability bit
    # will claim from next -- the exact collision class the reserved bit
    # exists to close permanently, and the class this project has already hit
    # twice before (T-1894's bit-0 canary, a later bit-1 canary).
    data = bytearray(base)
    flags = struct.unpack_from("<I", data, 16)[0]
    flags |= 0x80000000
    struct.pack_into("<I", data, 16, flags)
    return bytes(data)


def mk_truncated(base: bytes) -> bytes:
    # FileSizeMismatch -- artifact.cpp:322-327 (v1.5.0), also before the hash
    # check. header file_bytes keeps claiming the full length; the buffer
    # handed to the loader is shorter, matching a truncated download/copy.
    return bytes(base[: len(base) - 4096])


def mk_hash_mismatch(base: bytes) -> bytes:
    # IntegrityMismatch -- artifact.cpp:341 (v1.5.0). Flip one content byte deep inside
    # the Weights section (type 2, this fixture's section row 1) without
    # touching the header's stored hash, so the whole-file SHA-256 the loader
    # recomputes no longer matches the stored value. The header/section table
    # stay byte-identical to the valid fixture -- only content changed.
    data = bytearray(base)
    # Section row 1 in l2s0_valid_reference.sslm is (type=2 Weights, offset=640).
    weights_offset = struct.unpack_from("<Q", data, kHeaderBytes + 1 * kSectionDescBytes + 8)[0]
    data[weights_offset] ^= 0xFF
    return bytes(data)


def mk_overlapping_sections(base: bytes) -> bytes:
    # SectionOverlap -- artifact.cpp:390 (v1.5.0; or :423 for the sorted-neighbour
    # arm). Fires AFTER the hash check, so the hash must be recomputed
    # post-mutation for this to be the check that actually rejects the file.
    #
    # T-2226 fix round (routed from the build, the build record
    # SS8): the original mutation set the new offset to `row4_offset + row4_size - 64`, which
    # is only a bare arithmetic shift, not a value derived to satisfy the loader's OWN prior
    # checks. For this fixture's real geometry (row4_size = 30380, not a multiple of 64) that
    # shift landed on 118316, and 118316 % 64 == 44 -- Layer 1's per-section alignment check
    # (artifact.cpp ~line 334, `Misaligned`) runs in table order BEFORE either SectionOverlap
    # site, so it fired first and the fixture never reached the overlap check it claims to
    # trip. Fixed to derive an offset that is simultaneously:
    #   (1) 64-aligned (row5's own declared `alignment` field, unchanged, is 64);
    #   (2) inside row4's byte range [row4_offset, row4_end) -- the actual overlap; and
    #   (3) in-file and past the header/section-table region (asserted below, and again by
    #       ci/tests/test_fixture_integrity.py against the COMMITTED bytes, so this class of
    #       defect cannot recur silently).
    #
    # l2s0_valid_reference.sslm's own section table (enumerated 2026-08-22):
    #   row 4 = type 6  WeightScales,          offset  88000, size 30380 (end 118380)
    #   row 5 = type 7  CompositionConstants,  offset 118400, size  4319
    data = bytearray(base)
    row4_off_field = kHeaderBytes + 4 * kSectionDescBytes + 8
    row4_offset, row4_size = struct.unpack_from("<QQ", data, row4_off_field)
    row4_end = row4_offset + row4_size

    # The largest 64-aligned offset at or before row4's end. row4_size (30380) is not itself
    # a multiple of 64 (30380 % 64 == 44), so this is guaranteed strictly less than row4_end
    # and strictly greater than row4_offset for this fixture's real geometry -- both asserted
    # below rather than assumed.
    new_row5_offset = (row4_end // 64) * 64
    assert new_row5_offset % 64 == 0, "must satisfy row5's own declared 64-byte alignment"
    assert row4_offset < new_row5_offset < row4_end, (
        f"{new_row5_offset} must land strictly inside row4's range "
        f"[{row4_offset}, {row4_end}) to be a genuine overlap, not merely adjacent"
    )

    section_count = struct.unpack_from("<I", data, 12)[0]
    table_end = kHeaderBytes + section_count * kSectionDescBytes
    assert new_row5_offset >= table_end, "must land past the header/section-table region"

    row5_off_field = kHeaderBytes + 5 * kSectionDescBytes + 8
    row5_size = struct.unpack_from("<Q", data, row5_off_field + 8)[0]
    file_bytes = struct.unpack_from("<Q", data, 24)[0]
    assert new_row5_offset + row5_size <= file_bytes, "row5's new range must stay in-file"

    struct.pack_into("<Q", data, row5_off_field, new_row5_offset)
    recompute_hash(data)
    return bytes(data)


def main():
    src_path = sys.argv[1]
    out_dir = sys.argv[2]
    with open(src_path, "rb") as f:
        base = f.read()

    variants = {
        "l2s0_bad_magic.sslm": mk_bad_magic(base),
        "l2s0_unknown_flag_bit.sslm": mk_unknown_flag_bit(base),
        "l2s0_truncated.sslm": mk_truncated(base),
        "l2s0_hash_mismatch.sslm": mk_hash_mismatch(base),
        "l2s0_overlapping_sections.sslm": mk_overlapping_sections(base),
    }
    for name, data in variants.items():
        with open(f"{out_dir}/{name}", "wb") as f:
            f.write(data)
        print(f"wrote {name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
