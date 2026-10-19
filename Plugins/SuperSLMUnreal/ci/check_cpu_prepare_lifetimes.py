"""Read-only bounded C++ lifetime audit, not a C++ parser or sanitizer.

Enumerate every immediate destroy/return site in PrepareCpuState, derive local pointer/reference
aliases of its owned state, and reject uses of those aliases in the return expression after
destruction. Unsupported destroy/return shapes are rejected, not silently ignored.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path


def mask_noncode(text):
    # Preserve character positions/newlines while masking comments, strings and character literals.
    pattern = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    return pattern.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), text)


def audit(text):
    clean = mask_noncode(text)
    signature = re.search(r'\bstatic\s+FSuperSLMConfigureReport\s+PrepareCpuState\s*\(', clean)
    assert signature, 'PrepareCpuState definition is absent'
    start = clean.index('{', signature.end())
    depth, end = 1, start + 1
    while depth:
        assert end < len(clean), 'unterminated function'
        depth += (clean[end] == '{') - (clean[end] == '}')
        end += 1
    body = clean[start:end]
    roots = set(re.findall(r'\bFSuperSLMSubsystemState\s*\*\s*(\w+)\s*=\s*new\s+FSuperSLMSubsystemState\b', body))
    assert len(roots) == 1, 'expected exactly one owned prepared-state allocation'
    aliases = set(roots)
    declarations = list(re.finditer(r'\b\w+(?:\s*::\s*\w+)*(?:\s*<[^;{}]*>)?\s*[*&]\s*(\w+)\s*=\s*([^;]+);', body))
    while True:
        before = set(aliases)
        for declaration in declarations:
            if set(re.findall(r'\b\w+\b', declaration[2])) & aliases:
                aliases.add(declaration[1])
        if aliases == before:
            break
    # The captured teardown must actually own destruction; this is a condition of this audit.
    assert re.search(r'auto\s+TearDown\s*=\s*\[\s*' + next(iter(roots)) + r'\s*\]\s*\(\s*\)\s*\{\s*DestroyCpuState\(', body)
    sites = []
    destroy_pattern = re.compile(r'\bTearDown\s*\(\s*\)\s*;|\bdelete\s+(?:' + '|'.join(roots) + r')\s*;')
    for destroyed in destroy_pattern.finditer(body):
        tail = body[destroyed.end():]
        returned = re.match(r'\s*return\b([^;]*);', tail)
        line = clean.count('\n', 0, start + destroyed.start()) + 1
        if returned is None:
            sites.append({'destroy_line': line, 'unsupported': True, 'state_reads': []})
            continue
        expr = returned[1]
        used = sorted(set(re.findall(r'\b\w+\b', expr)) & aliases)
        sites.append({'destroy_line': line, 'return_line': clean.count('\n', 0, start + destroyed.end() + returned.start(1)) + 1,
                      'state_reads': used, 'expression': ' '.join(expr.split())})
    assert sites, 'no destruction sites were inspected'
    return {'sha256': hashlib.sha256(text.encode('utf-8')).hexdigest(), 'roots': sorted(roots),
            'aliases': sorted(aliases), 'sites': sites,
            'rejected_sites': [s for s in sites if s.get('unsupported') or s['state_reads']]}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.source.read_text(encoding='utf-8-sig'))
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
    raise SystemExit(1 if result['rejected_sites'] else 0)
