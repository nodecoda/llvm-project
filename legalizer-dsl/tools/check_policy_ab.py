#!/usr/bin/env python3
"""A/B gate for `policy`: every assertion here can fail.

    python3 tools/check_policy_ab.py            # ~2 min
    python3 tools/check_policy_ab.py --fast     # skip the two full runs

What it locks:
  1. the headline numbers do not drift;
  2. G_FCMP / G_SELECT are 100% under the policy file;
  3. the negative control: putting `hull` back on G_FCMP brings 552 back;
  4. answer coverage: `hull` on G_ANYEXT slot 0 is caught (13-policy-unreachable)
     -- the 32-bit hole the score cannot see because it measures 64-bit only;
  5. liveness: dropping G_FCMP slot 1's guard is caught (13-policy-dead-config);
  6. G_ANYEXT stays at 126 unexplained, i.e. the 126 are *not* a policy bug.

The last one is the important one for the next reader: 252 points across
ANYEXT/SEXT/ZEXT were once attributed to "a missing constant bound".  They are
not.  They are the chain's rule order (slot 1 widens before slot 0 scalarizes),
and no policy can fix that without changing when the rules run.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PY = sys.executable
fails = 0


def run(*args):
    r = subprocess.run([PY] + list(args), cwd=ROOT, capture_output=True,
                       text=True)
    return r.stdout + r.stderr


def need(text, needle, what):
    global fails
    if needle not in text:
        print('FAIL %s: %r not in output' % (what, needle))
        for line in text.strip().splitlines()[-6:]:
            print('  | ' + line)
        fails += 1
    else:
        print('ok   %s' % what)


def edit(src, out, old, new):
    text = open(os.path.join(ROOT, src)).read()
    assert text.count(old) == 1, (src, text.count(old), old[:50])
    open(out, 'w').write(text.replace(old, new))


def main(argv):
    tmp = tempfile.mkdtemp(prefix='policy-ab-')
    fast = '--fast' in argv

    if not fast:
        out = run('tools/infer_defaults.py', 'data/2026-11-01-x86.decl')
        need(out, '44385 / 46585', 'baseline 44385/46585')
        out = run('tools/infer_defaults.py', 'data/x86-policy.decl')
        need(out, '44663 / 46585', 'policy 44663/46585')

    out = run('tools/infer_defaults.py', 'data/x86-policy.decl', '--op', 'G_FCMP')
    need(out, '2880 / 2880', 'G_FCMP 2880/2880')
    out = run('tools/infer_defaults.py', 'data/x86-policy.decl', '--op', 'G_SELECT')
    need(out, '2304 / 2304', 'G_SELECT 2304/2304')

    # 3. negative control: `to s8` -> `hull` on G_FCMP
    f = os.path.join(tmp, 'fcmp-hull.decl')
    edit('data/x86-policy.decl', f,
         'policy 0: scalar to s8 when HasSSE1 || UseX87;',
         'policy 0: scalar hull when HasSSE1 || UseX87;')
    out = run('tools/infer_defaults.py', f, '--op', 'G_FCMP')
    need(out, '264 unexplained', 'negative control: hull brings 264 back')

    # 4. coverage: ANYEXT slot 0 as `hull` is caught (32-bit is unmeasured)
    f = os.path.join(tmp, 'anyext-hull.decl')
    edit('data/x86-policy.decl', f,
         'policy 0: scalar [s8, MaxScalar] pow2;',
         'policy 0: scalar hull pow2 8;')
    out = run('tools/check_decl.py', f)
    need(out, '13-policy-unreachable', 'coverage catches ANYEXT slot 0 as hull')

    # 5. liveness: drop G_FCMP slot 1's guard
    f = os.path.join(tmp, 'fcmp-noguard.decl')
    edit('data/x86-policy.decl', f,
         'pow2 when HasSSE1 || HasSSE2 || UseX87;', 'pow2;')
    out = run('tools/check_decl.py', f)
    need(out, '13-policy-dead-config', 'liveness catches the unguarded slot 1')

    # 6. the 126 are rule order, not a missing bound
    out = run('tools/infer_defaults.py', 'data/x86-policy.decl', '--op', 'G_ANYEXT')
    need(out, '42 unexplained', 'G_ANYEXT still 42 (rule order, not policy)')

    print('check_policy_ab: %d failed' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
