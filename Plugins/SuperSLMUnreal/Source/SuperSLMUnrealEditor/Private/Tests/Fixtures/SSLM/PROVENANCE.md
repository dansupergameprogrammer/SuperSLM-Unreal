# `.sslm` fixture provenance — T-2226 (L2-S0 red suite)

This directory holds eleven `.sslm` fixtures: one base artifact, `l2s0_valid_reference.sslm`, and
ten derived from it (five hostile variants and five provenance variants, below). Regenerating any
of them is deterministic; none is hand-edited.

## `l2s0_valid_reference.sslm` — 129,948 bytes

Generated 2026-08-22 by running SuperSLM's own `tools/_t2199_s8_synthetic_full_model_fixture.py`
at tag `v1.2.0` (commit `f409bda3fd`), extracted via `git archive v1.2.0 tools/ include/` into a
scratch tree, so that the script and the headers it reads are exactly the tag's.

This is not a hand-typed placeholder: it is the "Sec11 fixture model" shape Layer 1's own
`reference_pipeline` test suite exercises its Python forward pass against (real per-channel calibrated
weight scales, real RoPE tables, real KV-landing scales/reciprocals, real composition constants), run
through `convert_model.build_sections` — the same production writer path a real checkpoint conversion
uses. The identical script and config back Layer 1's own T-2199 Phase D suite, where this exact model
shape clears real engine construction. A fixture with a single placeholder weight does NOT clear that
bar, which is why this generator is used instead of a smaller hand-built one.

Contents (enumerated from the file's own section table, 2026-08-22): 11 sections — `Config` (0),
`Weights` (2), `Biases` (3), `RopeTables` (4), `WeightScales` (6), `CompositionConstants` (7),
`KvLandingScales` (8), `KvLandingReciprocals` (9), `SigmoidLut` (12), `DampedGreedyConstants` (42, the
optional `DGC1` payload — generator invoked with `enable_damped_greedy=True`), `SchemaMasks` (30, one
compiled schema `g5_minimal_one_field`). Header `flags = 0x2` (`kDampedGreedyArtifactConstantsFlag`
set, `kOptionGFusedKLandingFlag` clear). This is deliberate: it lets the same fixture serve both the
"valid artifact imports" cell and the dim-2(b) "optional DGC1 section is inert to a greedy-only plugin"
cell without a second fixture.

Regeneration: `git -C <SuperSLM checkout> archive v1.2.0 tools/ include/ | tar -x -C <scratch>`, then
`python <scratch>/tools/_t2199_s8_synthetic_full_model_fixture.py <out>.sslm` (needs `numpy`; no other
non-stdlib dependency). Per Layer 1's own S-HARDEN-5 discipline ("regenerated fresh every run, never
committed as a binary blob") this generator is not re-run by CI — it is documented here so the fixture's
origin is checkable. The fixture itself is committed because the vendored tree
(`Source/ThirdParty/SuperSLM`) is a later tag, `v1.9.0`, which carries the script but not
necessarily the same output, and the plugin's CI does not run Layer 1's Python tooling.

## The five hostile variants — `gen_hostile_fixtures.py`

Each is exactly ONE field mutated from `l2s0_valid_reference.sslm`, generated 2026-08-22 by
`gen_hostile_fixtures.py` (committed alongside these files — run it again against a re-generated
`l2s0_valid_reference.sslm` to reproduce byte-identical output, since every mutation is a deterministic
field write plus, where the loader's check order requires it, a recomputed SHA-256).
`l2s0_unknown_flag_bit.sslm` was regenerated twice on 2026-09-18 (T-2788, U0): first moving the
mutated bit from `0x4` to `0x8`, then -- on the maintainer's ruling, `docs/sslm_format.md`'s bit
allocation table (line 535 at v1.5.0) permanently reserves bit 31 (`0x80000000`) as the only bit
any unknown-flag fixture may use -- to `0x80000000`. The other four hostile variants are
unchanged by either regeneration (re-run and diffed byte-for-byte against the prior commit to
confirm).

Every mutation and its expected first-failing check is derived directly from `docs/sslm_format.md`'s
byte layout table and cross-checked against the loader's own validation order, read at
`git show v1.5.0:src/artifact.cpp` lines 248–344 (`superslm::SslmArtifact::OpenFromMemory`; T-2788
re-pin, 2026-09-18 -- the check order is unchanged from v1.2.0, only the line numbers moved) — the
same "derive from the spec, never from the parser" discipline SuperSLM's own
`tests/sslm_model_hostile_fixtures.h` states for its manifest fixtures:

| File | Field mutated | Expected first-failing check | Fires before or after the hash check? |
|---|---|---|---|
| `l2s0_bad_magic.sslm` | byte 0 of `magic` | `BadMagic` | before |
| `l2s0_unknown_flag_bit.sslm` | `flags` byte 16, bit `0x80000000` (bit 31) set -- the bit `docs/sslm_format.md` permanently reserves for exactly this fixture class (moved from `0x4`, then `0x8`, at T-2788, U0: `0x4` became `kQkNormFusedKChannelTableFlag`, a known bit, at v1.5.0; `0x8` sits in the sequentially-allocated range a future capability bit would claim next) | `BadHeader` (unknown flag bit) | before |
| `l2s0_truncated.sslm` | last 4096 bytes of the file dropped; `file_bytes` header field unchanged | `FileSizeMismatch` | before |
| `l2s0_hash_mismatch.sslm` | one content byte inside the `Weights` section (offset 640) flipped; header untouched | `IntegrityMismatch` | at |
| `l2s0_overlapping_sections.sslm` | section row 5 (`CompositionConstants`)'s `offset` field set to 118336 (the largest 64-aligned offset at or before row 4's end, `(row4_offset+row4_size)//64*64` — see the T-2226 fix-round note below); hash recomputed | `SectionOverlap` (rows 4, 5) | after (hash still verifies) |

**T-2226 fix round, 2026-08-22 (routed from the build,
the build record §8):** the original `l2s0_overlapping_sections.sslm`
set row 5's offset to `row4_offset + row4_size - 64` = 118316. **118316 % 64 == 44** — Layer 1's
per-section alignment check (`src/artifact.cpp` line 369 at v1.5.0, `Misaligned`) runs in table order
BEFORE either `SectionOverlap` site (390, 423 at v1.5.0), so the fixture tripped `Misaligned` on row 5, never the
`SectionOverlap` it claimed to construct. The plugin under test reported Layer 1's TRUE status for the
bytes it was actually handed; the defect was the fixture, not the diagnostic it was meant to exercise.
Fixed by deriving the offset as `(row4_offset + row4_size) // 64 * 64` = 118336 — the largest
64-aligned offset at or before row 4's end — which is simultaneously (1) in-file, (2) past the
header/section-table region, (3) 64-aligned (row 5's own declared alignment field), and (4) strictly
inside row 4's byte range `[88000, 118380)`, so it is a genuine overlap and not merely adjacent. Every
one of the four properties is now asserted twice: inline in `gen_hostile_fixtures.py::
mk_overlapping_sections` at generation time, and structurally against the COMMITTED bytes by
`ci/tests/test_fixture_integrity.py::test_overlapping_sections_row5_offset_satisfies_all_four_properties`
— which also formalizes the check-order re-implementation below as a real, committed, always-run
pytest module (`test_every_fixture_trips_exactly_its_claimed_check`) rather than only an authoring-time
hand check, so this class of defect cannot recur silently.

**Self-check performed at authoring time (not a substitute for the real C++ loader, which the
automation tests exercise):** an independent Python re-implementation of `OpenFromMemory`'s check
sequence, written directly from the same source lines and run against the base and the five hostile
variants, confirms each fixture is caught by exactly the check
named above and no earlier one. `l2s0_valid_reference.sslm` alone passes every check the
re-implementation covers (through `SectionOverlap`; it does not reach `SslmModel::Load`'s deeper
semantic checks, which the real loader performs after `OpenFromMemory` returns `Ok`). This check is now
also `ci/tests/test_fixture_integrity.py::test_every_fixture_trips_exactly_its_claimed_check`, run on
every suite invocation rather than only by hand.

## The five provenance variants — `gen_provenance_fixtures.py`

Each inserts one `Provenance` section (type 1, dtype Raw: UTF-8 bytes whose only length is the
section's `byte_size`) into `l2s0_valid_reference.sslm`'s eleven sections, then repacks the whole
file (sections laid out in order, each at its declared alignment) and recomputes the SHA-256 the
way the loader does. The byte layout comes from `docs/sslm_format.md`, never from the plugin's own
reader. `gen_provenance_fixtures.py` is committed alongside them; run it against
`l2s0_valid_reference.sslm` to reproduce them byte for byte. `SuperSLMProvenanceTests.cpp` reads
them.

| File | Provenance section |
|---|---|
| `l2s0_provenance_normal.sslm` | A realistic JSON blob, second in the section list, right after `Config`, so an overread lands on real section bytes |
| `l2s0_provenance_max_no_nul.sslm` | 256 printable bytes with no NUL, last in the file, so an overread runs off the end of the allocation |
| `l2s0_provenance_embedded_nul.sslm` | A NUL inside the content, not last, so truncation at the NUL is caught separately from an overread |
| `l2s0_provenance_zero_length.sslm` | `byte_size` 0; must import, with an empty provenance string |
| `l2s0_provenance_oversize.sslm` | 16 KiB with no NUL, last in the file |
