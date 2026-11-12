#!/usr/bin/env python3
"""What German soldiers will call each prop in a map.

    python3 landmark_report.py dod_somemap.bsp

Reads the static props and prop entities out of the .bsp and runs each model name through the
same landmark rules the game uses (parsed straight out of german_chatter.cpp, so the two can't
drift apart). Prints every model with its count and the callout it gets."""
import re, struct, sys, os, collections

HERE = os.path.dirname(os.path.abspath(__file__))
CPP = os.path.join(HERE, '..', 'sp', 'src', 'game', 'server', 'dodsp', 'german_chatter.cpp')

def load_rules():
    src = open(CPP).read()
    body = src[src.index('s_LandmarkRules[] ='):]
    body = body[:body.index('};')]
    rules = []
    for word, lm, flags in re.findall(r'\{\s*"([^"]*)",\s*"([^"]*)",\s*([A-Z_ |]+)\}', body):
        rules.append((word, lm, set(f.strip() for f in flags.split('|'))))
    return rules

def words(name):
    base = name.rsplit('/', 1)[-1].split('.')[0].lower()
    out = []
    for w in re.findall(r'[a-z0-9]+', base)[:8]:
        w = w[:31]
        m = w
        if len(m) > 1 and m[-1].isalpha() and m[-2].isdigit(): m = m[:-1]
        m = m.rstrip('0123456789')
        out.append(m if m else w)
    return out

def matches(w, word, flags):
    n = len(word)
    if n > len(w): return False
    if 'LM_WORD' in flags and w in (word, word + 's', word + 'es'): return True
    if 'LM_PREFIX' in flags and w.startswith(word): return True
    if 'LM_SUFFIX' in flags and (w.endswith(word) or (w.endswith('s') and w[:-1].endswith(word))): return True
    return False

def landmark(name, rules):
    if not name or name.startswith('*') or 'skybox' in name.lower(): return None, 'skybox/brush'
    ws = words(name)
    for word, lm, flags in rules:
        for w in ws:
            if matches(w, word, flags):
                return (lm or None), ('dressing "%s"' % word if not lm else 'word "%s"' % word)
    return None, ''

def props(path):
    f = open(path, 'rb').read()
    lumps = [struct.unpack_from('<iii4s', f, 8 + 16 * i) for i in range(64)]
    o, l, _, _ = lumps[0]
    ents = [dict(re.findall(r'"([^"]*)"\s+"([^"]*)"', b)) for b in re.findall(r'\{(.*?)\}', f[o:o + l].decode('latin1'), re.S)]
    o, l, _, _ = lumps[35]
    out = []
    for i in range(struct.unpack_from('<i', f, o)[0]):
        gid, _, gver, gofs, glen = struct.unpack_from('<iHHii', f, o + 4 + 16 * i)
        if struct.pack('>i', gid) != b'sprp': continue
        p = gofs
        nd = struct.unpack_from('<i', f, p)[0]; p += 4
        names = [f[p + 128 * k:p + 128 * (k + 1)].split(b'\0')[0].decode('latin1').lower() for k in range(nd)]; p += 128 * nd
        p += 4 + 2 * struct.unpack_from('<i', f, p)[0]
        n = struct.unpack_from('<i', f, p)[0]; p += 4
        sz = (gofs + glen - p) // n if n else 0
        out += [('static', names[struct.unpack_from('<H', f, p + sz * k + 24)[0]]) for k in range(n)]
    out += [(e['classname'], e['model'].lower()) for e in ents if e.get('classname', '').startswith('prop_') and e.get('model')]
    return out

if __name__ == '__main__':
    rules = load_rules()
    c = collections.Counter(props(sys.argv[1]))
    rows = []
    for (kind, model), n in c.items():
        lm, why = landmark(model, rules)
        rows.append((lm or '-', model, kind, n, why))
    for lm, model, kind, n, why in sorted(rows):
        print('%-12s %4d  %-58s %s %s' % (lm, n, model, kind, why))
