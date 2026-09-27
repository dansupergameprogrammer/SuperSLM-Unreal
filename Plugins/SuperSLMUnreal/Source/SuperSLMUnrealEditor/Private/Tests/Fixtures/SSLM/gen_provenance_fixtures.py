"""T-2226 fix round 6 (T-2249 N4). Builds the type-1 (Provenance) fixture family used to
prove/close N2 (ExtractProvenanceJson's out-of-bounds read). Byte layout derived directly from
docs/sslm_format.md (header 64 bytes, section table 40 bytes/row, dtype Json = Raw
UTF-8 bytes with NO internal sub-format -- the section's own byte_size is the only length
signal, which is exactly the field the current code discards). Re-checked unchanged at v1.5.0
(T-2788 re-pin, 2026-09-18): the header/section-table byte layout and the Json/Raw dtype
equivalence are untouched by the v1.2.0->v1.5.0 delta (only the `flags` known-bit mask and the
section-type table grew). Never derived from the plugin's own (buggy) ExtractProvenanceJson.

General approach: parse l2s0_valid_reference.sslm's existing 11 sections into
(type, dtype, alignment, raw_bytes) tuples, INSERT a new Provenance (type 1, dtype 0/Raw)
section at a controlled position in the list, then repack the WHOLE file from scratch --
header + an N-row table + sections laid out sequentially, each starting at the next multiple
of its own declared alignment. This never reuses old byte offsets (the table grows by one row,
so every existing section's offset must move) and recomputes the SHA-256 exactly as the real
loader does (whole file, hash field zeroed).
"""
import hashlib
import struct
from pathlib import Path

kHeaderBytes = 64
kSectionDescBytes = 40
kIntegrityHashOffset = 32
kIntegrityHashBytes = 32


def parse_sections(data: bytes):
    section_count = struct.unpack_from("<I", data, 12)[0]
    out = []
    for i in range(section_count):
        row = kHeaderBytes + i * kSectionDescBytes
        stype, dtype = struct.unpack_from("<II", data, row)
        off, sz, ec = struct.unpack_from("<QQQ", data, row + 8)
        align, res = struct.unpack_from("<II", data, row + 32)
        out.append({
            "type": stype, "dtype": dtype, "align": align,
            "elem_count": ec, "data": bytes(data[off:off + sz]),
        })
    return out


def pack_artifact(sections, flags: int) -> bytes:
    """sections: list of dicts with type/dtype/align/elem_count/data. Table row order == list
    order; byte layout order == list order too (sequential packing), matching how the upstream
    SuperSLM repository's own tools lay a fresh artifact out."""
    section_count = len(sections)
    table_end = kHeaderBytes + section_count * kSectionDescBytes

    # First pass: compute each section's offset by packing sequentially from table_end,
    # each aligned to its own declared alignment.
    cursor = table_end
    offsets = []
    for s in sections:
        align = s["align"]
        if cursor % align != 0:
            cursor += align - (cursor % align)
        offsets.append(cursor)
        cursor += len(s["data"])
    file_bytes = cursor

    buf = bytearray(file_bytes)
    # Header
    buf[0:4] = b"SSLM"
    struct.pack_into("<I", buf, 4, 2)              # format_version
    struct.pack_into("<I", buf, 8, kHeaderBytes)    # header_bytes
    struct.pack_into("<I", buf, 12, section_count)  # section_count
    struct.pack_into("<I", buf, 16, flags)          # flags
    struct.pack_into("<I", buf, 20, 0)              # reserved0
    struct.pack_into("<Q", buf, 24, file_bytes)      # file_bytes
    # integrity_sha256 filled in below, after everything else is written

    # Table + section bytes
    for i, (s, off) in enumerate(zip(sections, offsets)):
        row = kHeaderBytes + i * kSectionDescBytes
        struct.pack_into("<I", buf, row + 0, s["type"])
        struct.pack_into("<I", buf, row + 4, s["dtype"])
        struct.pack_into("<Q", buf, row + 8, off)
        struct.pack_into("<Q", buf, row + 16, len(s["data"]))
        struct.pack_into("<Q", buf, row + 24, s["elem_count"])
        struct.pack_into("<I", buf, row + 32, s["align"])
        struct.pack_into("<I", buf, row + 36, 0)  # reserved
        buf[off:off + len(s["data"])] = s["data"]

    # Hash: whole file, hash field zeroed, matching the loader's own algorithm exactly
    # (verified identical to git show v1.5.0:src/artifact.cpp's OpenFromMemory, re-checked at
    # the T-2788 re-pin -- see PROVENANCE.md).
    tmp = bytearray(buf)
    tmp[kIntegrityHashOffset:kIntegrityHashOffset + kIntegrityHashBytes] = b"\x00" * 32
    digest = hashlib.sha256(bytes(tmp)).digest()
    buf[kIntegrityHashOffset:kIntegrityHashOffset + kIntegrityHashBytes] = digest
    return bytes(buf)


def make_provenance_section(content: bytes, elem_count=None):
    return {
        "type": 1,       # SslmSectionType::Provenance
        "dtype": 0,      # SslmDtype::Raw (Json sections use dtype Raw -- UTF-8 bytes)
        "align": 64,
        "elem_count": elem_count if elem_count is not None else len(content),
        "data": content,
    }


def main():
    src_path = Path(__file__).parent / "l2s0_valid_reference.sslm"
    out_dir = Path(__file__).parent
    base = src_path.read_bytes()
    base_sections = parse_sections(base)
    # This fixture's flags field (0x2, DampedGreedyConstants) is unrelated to provenance;
    # carried through unchanged so every variant stays a faithful, otherwise-valid model.
    flags = struct.unpack_from("<I", base, 16)[0]

    variants = {}

    # (1) normal: a realistic provenance JSON blob, INSERTED MID-LIST (position 1, right
    # after Config) so any overread would land on real subsequent section bytes -- silently
    # wrong content, the failure mode §3's own text describes ("a wrong ProvenanceJson in the
    # Details panel, silently").
    normal_json = (
        b'{"checkpoint":"shopkeeper-ref-1.5b","license":"Apache-2.0",'
        b'"source_hash":"9f2b7c4a1e6d3f80"}'
    )
    sections = list(base_sections)
    sections.insert(1, make_provenance_section(normal_json))
    variants["l2s0_provenance_normal.sslm"] = pack_artifact(sections, flags)

    # (2) non-NUL-terminated at max length, LAST section in the file: fills its own declared
    # byte_size completely with printable, non-NUL bytes, and is the final section written --
    # so a Strlen-style overread runs off the actual end of the FMemory::Malloc allocation,
    # which is N2's own worst-case ("As the final section it runs off the end of the
    # FMemory::Malloc allocation"). 256 bytes: large enough that an overread is unambiguous
    # in a diff, small enough to keep the fixture file small.
    max_len_content = bytes((i % 94) + 33 for i in range(256))  # printable ASCII, no 0x00
    assert 0 not in max_len_content
    sections = list(base_sections)
    sections.append(make_provenance_section(max_len_content))
    variants["l2s0_provenance_max_no_nul.sslm"] = pack_artifact(sections, flags)

    # (3) embedded NUL, mid-file (not last): isolates the TRUNCATION failure mode from the
    # out-of-bounds-read failure mode. Correct behaviour preserves content through and past
    # the embedded NUL, up to byte_size; the current code's FString(const WIDECHAR*)
    # constructor (implicit Strlen) truncates at it.
    embedded_nul = b'{"checkpoint":"trunc' + b"\x00" + b'ated-if-buggy","source_hash":"aa"}'
    assert b"\x00" in embedded_nul
    sections = list(base_sections)
    sections.insert(1, make_provenance_section(embedded_nul))
    variants["l2s0_provenance_embedded_nul.sslm"] = pack_artifact(sections, flags)

    # (4) zero-length: byte_size == 0. Already correctly handled by the existing
    # `byte_size > 0` guard -- a must-accept boundary fixture (ProvenanceJson must be "" and
    # the import must not fail), not a bug reproduction, but part of the family since it is
    # the other numeric extreme from (2)/(5).
    sections = list(base_sections)
    sections.insert(1, make_provenance_section(b""))
    variants["l2s0_provenance_zero_length.sslm"] = pack_artifact(sections, flags)

    # (5) oversize, LAST section: a much larger provenance blob (16 KiB) than any real
    # checkpoint/license/hash JSON would need, non-NUL throughout, positioned last -- the
    # widest-reach version of (2), stressing both the overread's absolute size and any
    # fixed-size intermediate buffer a future fix might introduce.
    oversize_content = bytes((i % 94) + 33 for i in range(16 * 1024))
    assert 0 not in oversize_content
    sections = list(base_sections)
    sections.append(make_provenance_section(oversize_content))
    variants["l2s0_provenance_oversize.sslm"] = pack_artifact(sections, flags)

    for name, data in variants.items():
        (out_dir / name).write_bytes(data)
        print(f"wrote {name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
