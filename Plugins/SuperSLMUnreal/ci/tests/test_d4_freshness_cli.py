"""CLI mechanism regressions for freshness; these are explicitly synthetic traces.

Independent authentic 38-case controls and their original UE transport timestamps
are retained in the paging instrument review evidence, separately from this suite.
No witness helper or production constant supplies expected verdicts here.
"""
import csv
import os
import subprocess
import sys
from pathlib import Path

import pytest


CALL_FIELDS = ('call_id,block,mode,arm,depth,rep,layer,start_s,end_s,elapsed_ms,'
               'foreign_pct,foreign_peak_pct,foreign_coverage,first_sample,last_sample').split(',')
SAMPLE_FIELDS = 'sample_id,block,start_s,end_s'.split(',')
PREFIX = 'SuperSLM.EstimateCosts: '


def write_rows(path, fields, rows):
    with path.open('w', encoding='utf-8', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)


def make_series(root, defect):
    """Two traces inside separate one-second wall-clock envelopes, with fresh ids."""
    logs = []
    for index, run_id in enumerate(('first', 'second')):
        folder = root / run_id
        folder.mkdir()
        for mode in (0, 1):
            rows = [dict(zip(CALL_FIELDS, [str(i), f'{run_id}-m{mode}', str(mode),
                    'finish', '1', str(i), '-1', str(.1+i*.2+mode*.4),
                    str(.15+i*.2+mode*.4), '50.000000000', '0', '0', '1', '0', '1']))
                    for i in range(2)]
            if index == 1 and mode == 1:
                if defect == 'call-counter': rows[-1]['call_id'] = '3'
                if defect == 'stale-call': rows[-1]['block'] = 'first-m1'
                if defect == 'wrong-mode': rows[-1]['block'] = 'second-m0'
                if defect == 'empty-call-block': rows[-1]['block'] = ''
                if defect == 'call-nan': rows[-1]['end_s'] = 'nan'
                if defect == 'call-inf': rows[-1]['start_s'] = 'inf'
                if defect == 'call-reversed': rows[-1].update(start_s='.9', end_s='.8')
                if defect == 'call-negative': rows[0]['start_s'] = '-.000000001'
                if defect == 'call-upper-ok': rows[-1].update(start_s='.95', end_s='1.001000001')
                if defect == 'call-upper-bad': rows[-1].update(start_s='.95', end_s='1.001000002')
            if index == 1 and defect == 'positive-origin':
                for row in rows:
                    row.update(start_s=str(float(row['start_s'])+10000), end_s=str(float(row['end_s'])+10000))
            write_rows(folder / f'mode{mode}.calls.csv', CALL_FIELDS, rows)
        samples = [dict(sample_id=str(i), block=run_id, start_s=str(i*.5), end_s=str((i+1)*.5)) for i in range(2)]
        if index == 1:
            if defect == 'sample-counter': samples[-1]['sample_id'] = '3'
            if defect == 'stale-sample': samples[-1]['block'] = 'first'
            if defect == 'empty-sample-block': samples[-1]['block'] = ''
            if defect == 'sample-nan': samples[-1]['start_s'] = 'nan'
            if defect == 'sample-inf': samples[-1]['end_s'] = 'inf'
            if defect == 'sample-reversed': samples[-1].update(start_s='.9', end_s='.8')
            if defect == 'sample-negative': samples[0]['start_s'] = '-.000000001'
            if defect == 'sample-upper-ok': samples[-1]['end_s'] = '1.001000001'
            if defect == 'sample-upper-bad': samples[-1]['end_s'] = '1.001000002'
            if defect == 'positive-origin':
                for row in samples:
                    row.update(start_s=str(float(row['start_s'])+10000), end_s=str(float(row['end_s'])+10000))
        write_rows(folder / 'samples.csv', SAMPLE_FIELDS, samples)
        header_time = f'[2026.10.16-04.00.{index*2:02d}:000]'
        complete_time = f'[2026.10.16-04.00.{index*2+1:02d}:000]'
        if index == 1:
            if defect == 'missing-header': header_time = ''
            if defect == 'missing-complete': complete_time = ''
            if defect == 'invalid-header': header_time = '[2026.13.16-04.00.02:000]'
            if defect == 'invalid-complete': complete_time = '[2026.13.16-04.00.03:000]'
            if defect == 'backward': complete_time = '[2026.10.16-04.00.01:999]'
        start = header_time+'[1]LogSuperSLM: Display: '+PREFIX
        finish = complete_time+'[2]LogSuperSLM: Display: '+PREFIX
        lines = [start+'CALIBRATION ESTIMATE — not verified', start+'run id='+run_id,
                 start+'files '+str(folder), start+'coverage samples=2 uncovered_calls=0',
                 finish+'run status=complete elapsed_s=1.000']
        if index == 1 and defect == 'duplicate-complete': lines.append(lines[-1])
        if index == 1 and defect in ('complete-suffix', 'complete-assignment'):
            replacement = 'completeBAD' if defect == 'complete-suffix' else 'complete=garbage'
            lines[-1] = lines[-1].replace('status=complete ', 'status='+replacement+' ')
        logs.extend(lines)
    return '\n'.join(logs)+'\n'


@pytest.mark.parametrize('encoding', ['utf-8', 'utf-16'])
@pytest.mark.parametrize('case,expected', [('healthy', 0), ('call-upper-ok', 0), ('sample-upper-ok', 0)] +
    [(name, 1) for name in ('call-counter', 'sample-counter', 'stale-call', 'wrong-mode',
     'empty-call-block', 'stale-sample', 'empty-sample-block', 'positive-origin',
     'missing-header', 'missing-complete', 'invalid-header', 'invalid-complete', 'backward',
     'duplicate-complete', 'complete-suffix', 'complete-assignment', 'call-nan', 'call-inf', 'call-reversed', 'call-negative',
     'call-upper-bad', 'sample-nan', 'sample-inf', 'sample-reversed', 'sample-negative', 'sample-upper-bad')])
def test_freshness_cli(tmp_path, case, expected, encoding):
    log = tmp_path / 'series.log'
    log.write_text(make_series(tmp_path, case), encoding=encoding)
    env = os.environ.copy()
    env.update(PYTHONDONTWRITEBYTECODE='1', PYTHONIOENCODING='utf-8')
    result = subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('d4_estimate_witness.py')),
                             'fresh', '--log', str(log)], capture_output=True, text=True, encoding='utf-8', env=env)
    assert result.returncode == expected, result.stdout + result.stderr
    assert 'fresh:' in result.stdout and 'Traceback' not in result.stderr
    assert ('FAIL:' in result.stdout) == (expected == 1)
