"""Build scripts/game_sounds_german_chatter.txt from the de/<voice>/ge_<voice>_<category>_NN.wav set."""
import zipfile, re, collections, sys
z = zipfile.ZipFile(sys.argv[1]); out = sys.argv[2]
files = [x for x in z.namelist() if x.endswith('.wav')]
NATIONS = ['us', 'uk', 'ru']
pat = re.compile(r'^ge_(\d)_(.+?)(?:_(us|uk|ru))?_(\d+)[a-z]?\.wav$')
table = collections.defaultdict(lambda: collections.defaultdict(list))   # (voice, base) -> nation -> files
for f in files:
    parts = f.split('/'); name = parts[-1]; voice = parts[1]
    m = pat.match(name)
    if not m or m.group(1) != voice:
        print('SKIP', f); continue
    base = 'order_cover_tree' if m.group(2) == 'order_cover_trees' else m.group(2)
    table[(voice, base)][m.group(3)].append(f'dodsp/chatter/de/{voice}/{name}')
lines = ['// German battle chatter (3 voices). Generated from the de/ voice set - do not edit by hand.',
         '// Entry: GermanChatter.v<voice>.<category>.<enemy nation>  =  the plain lines + the lines naming that nation.',
         '// Wave files go in sound/dodsp/chatter/de/<voice>/', '']
n = 0; bases = collections.defaultdict(set)
for (voice, base) in sorted(table):
    byn = table[(voice, base)]
    for nat in NATIONS:
        waves = sorted(byn.get(None, [])) + sorted(byn.get(nat, []))
        if not waves: continue
        n += 1; bases[base].add(voice)
        lines += [f'"GermanChatter.v{voice}.{base}.{nat}"', '{', '\t"channel"\t\t"CHAN_VOICE"', '\t"volume"\t\t"1.0"',
                  '\t"soundlevel"\t"SNDLVL_90dB"', '\t"pitch"\t\t\t"PITCH_NORM"', '\t"rndwave"', '\t{']
        lines += [f'\t\t"wave"\t"{w}"' for w in waves]
        lines += ['\t}', '}', '']
open(out, 'w', newline='\r\n').write('\n'.join(lines))
print(n, 'entries;', len(bases), 'categories')
for b in sorted(bases): print(' ', b, sorted(bases[b]))
