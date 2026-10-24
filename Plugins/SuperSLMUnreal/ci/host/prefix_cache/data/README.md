# Test data

`refund_oracle_trace.txt` is a recorded run of the precache plan's executed model of the
deferred-refund admission (plan rev 1.17 §5.11 (a), "Executed"), rule N with the rev 1.16 and
1.17 fixes, root entries only, releases applied in random order: 1,500 cases, seed 17. The
generator and the model live with the plan's records, not in this plugin; the trace is the only
part the suite needs. `test_refund_oracle.cpp` replays it against `FSuperSLMResidencyCore`.

Line format, one step per line: `CASE <budget>`, `ENTRY <key> <charge> <leased>`,
`REQ <request> <key> <charge> <hit|now|wait|refused|coalesced> <evicted keys or ->`,
`APPLY <eviction index in this case> <admitted requests or ->`, `LEASE <key> <0|1>`,
`COMPLETE <request> <resident charge>`, `LEDGER <charged total>`, `END`.
