#!/usr/bin/env python3
"""P2's host mutants on the core plugin 1.4 keeps (schema fast-decode plan rev 1.4.4, §12): XG27 and XG27c, the carry.
Ruling 40 (red-suite-mr6.md) retires the rest with P2's Canonical GPU core: XG10, XG16(b, c), XG20(b), XG23, XG24(b),
XG27b (wrapper v2), XG28, XG29(b), XG32(b), XG33, XG34, XG35(b), XG38(b), XG39, XG40 and XG41. XG33 and XG40 are
re-pointed at the multi-row Exact path as automation cells R40.1 and R40.2. Each is an exact edit of SuperSLMGpuSchemaJumpCore.cpp whose anchors must occur once
(a drifted anchor is reported, never applied elsewhere). The script copies Source/ and ci/ into WORK, builds this suite
against each mutated core with g++, and reports KILLED (a cell failed) or SURVIVED.

  run_gpu_mutants.py WORK [XGn ...]
"""
import os, shutil, subprocess, sys
SRC = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
if len(sys.argv) < 2:
    sys.exit(__doc__)
W = os.path.abspath(sys.argv[1])
sys.argv = sys.argv[1:]
CORE = 'Source/SuperSLMUnreal/Private/Core/SuperSLMGpuSchemaJumpCore.cpp'
M = {
 'XG27': [('\t\t\t\t\tE.bForced = D.bForced;\n', '')],
 'XG27c': [('if (R.Ids.empty())\n\t\t\t\t{', 'if (true)\n\t\t\t\t{')],
}
names = sys.argv[1:] or list(M)
tree = os.path.join(W, 'tree')
if not os.path.isdir(tree):
    os.makedirs(tree)
    shutil.copytree(os.path.join(SRC, 'Source'), os.path.join(tree, 'Source'))
    shutil.copytree(os.path.join(SRC, 'ci'), os.path.join(tree, 'ci'))
orig = open(os.path.join(SRC, CORE), newline='').read()
crlf = '\r\n' in orig
build = os.path.join(W, 'build')
for n in names:
    text = orig
    for old, new in M[n]:
        o = old.replace('\n', '\r\n') if crlf else old
        nn = new.replace('\n', '\r\n') if crlf else new
        c = text.count(o)
        if c != 1:
            print('%s: anchor occurs %d times' % (n, c)); break
        text = text.replace(o, nn, 1)
    else:
        open(os.path.join(tree, CORE), 'w', newline='').write(text)
        if not os.path.isdir(build):
            subprocess.run(['cmake', '-S', os.path.join(tree, 'ci/host/schema_jump'), '-B', build], stdout=subprocess.DEVNULL, env=dict(os.environ, CXX='g++'))
        b = subprocess.run(['cmake', '--build', build, '-j4', '--target', 'schema_jump_host_tests'], capture_output=True, text=True)
        if b.returncode != 0:
            print('%s: BUILD FAIL %s' % (n, b.stdout[-400:])); continue
        r = subprocess.run([os.path.join(build, 'schema_jump_host_tests')], capture_output=True, text=True)
        fails = [l.split()[1] for l in r.stdout.splitlines() if l.startswith('[FAIL]')]
        print('%s: %s (%s)' % (n, 'KILLED' if fails else 'SURVIVED', ', '.join(fails[:6])))
open(os.path.join(tree, CORE), 'w', newline='').write(orig)
print('run_gpu_mutants: %d mutants run' % len(names))
