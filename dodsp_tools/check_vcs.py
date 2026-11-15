#!/usr/bin/env python3
"""DOD-SP build guard: every compiled .vcs must have exactly the combo layout the shader DLL
expects (its fxctmp9/vshtmp9 .inc header). A mismatch means the DLL indexes the wrong shader
programs - flat, black or garbage surfaces in game - so the build fails instead.

usage: check_vcs.py <dir with .vcs files> <stdshaders dir>"""
import os, re, struct, sys

def vcs_counts(path):
    with open(path, 'rb') as f:
        ver, total, dyn = struct.unpack('<iii', f.read(12))
    return ver, total, dyn

def inc_counts(stdshaders, base):
    for sub in ('fxctmp9', 'vshtmp9'):
        p = os.path.join(stdshaders, sub, base + '.inc')
        if os.path.exists(p):
            break
    else:
        return None
    s = open(p, errors='ignore').read()
    out = {}
    for kind in ('Static', 'Dynamic'):
        m = re.search(r'class \w+_' + kind + r'_Index(.*?)\n\};', s, re.S)
        body = m.group(1) if m else ''
        terms = re.findall(r'\(\s*(\d+)\s*\*\s*m_n(\w+)\s*\)', body)
        rng = {}
        for var, lo, hi in re.findall(r'void Set(\w+)\(\s*int i\s*\)\s*\{\s*Assert\(\s*i\s*>=\s*(-?\d+)\s*&&\s*i\s*<=\s*(-?\d+)', body):
            rng[var] = int(hi) - int(lo) + 1
        out[kind] = max([int(mu) * rng.get(v, 1) for mu, v in terms] or [1])
    total = out['Static'] if out['Static'] > 1 else out['Dynamic']
    return total, out['Dynamic']

def main():
    vcsdir, stdshaders = sys.argv[1], sys.argv[2]
    files = sorted(f for f in os.listdir(vcsdir) if f.lower().endswith('.vcs'))
    if not files:
        print('INVARIANT: no .vcs files were produced'); return 1
    bad = 0
    for f in files:
        base = f[:-4]
        ver, total, dyn = vcs_counts(os.path.join(vcsdir, f))
        inc = inc_counts(stdshaders, base)
        if inc is None:
            print('INVARIANT: %s has no .inc header - the DLL cannot use it' % f); bad += 1; continue
        ok = (total, dyn) == inc and ver == 6
        print('%s %-48s vcs v%d %d/%d  dll %d/%d' % ('ok ' if ok else 'BAD', f, ver, total, dyn, inc[0], inc[1]))
        bad += not ok
    if bad:
        print('INVARIANT: %d shader(s) do not match the DLL' % bad); return 1
    print('all %d shaders match the DLL' % len(files)); return 0

if __name__ == '__main__':
    sys.exit(main())
