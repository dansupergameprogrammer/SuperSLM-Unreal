"""An Unreal failure dump must not manufacture a second estimate invocation."""

import pytest

import d4_estimate_witness as witness


@pytest.mark.parametrize("encoding", ["utf-8", "utf-16"])
def test_failure_dump_echoes_preserve_real_run_identity(tmp_path, encoding):
    log = tmp_path / "editor.log"
    # Actual LogSuperSLM output, then the automation controller's failure-dump copies,
    # followed by another actual invocation. Both editor encodings occur on Windows.
    log.write_text(
        f"[time][1]LogSuperSLM: Display: {witness.HEADER}\n"
        f"[time][1]LogSuperSLM: Display: {witness.PREFIX}run id=first\n"
        f"[time][2]LogAutomationController: LogSuperSLM: {witness.HEADER} [log]\n"
        f"[time][2]LogAutomationController: LogSuperSLM: {witness.PREFIX}run id=first [log]\n"
        f"[time][3]LogSuperSLM: Display: {witness.HEADER}\n"
        f"[time][3]LogSuperSLM: Display: {witness.PREFIX}run id=second\n",
        encoding=encoding,
    )
    runs = [witness.parse_run(lines) for lines in witness.estimate_runs(log)]
    assert [run["run"]["id"] for run in runs] == ["first", "second"]
    assert all(" [log]" not in line for run in runs for line in run["lines"])
