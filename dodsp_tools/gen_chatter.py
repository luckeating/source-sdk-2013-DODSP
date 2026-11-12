"""Build scripts/game_sounds_german_chatter.txt from the de/<voice>/ge_<voice>_<category>_NN.wav set,
plus (optional 3rd argument) the pain/death set: germaninjure/gen_<m|n|o>_painN.wav, germandeath/gen_<m|n|o>_deathN.wav.
Usage: python3 gen_chatter.py de.zip out.txt [moresounds.zip]"""
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
if len(sys.argv) > 3:
    z2 = zipfile.ZipFile(sys.argv[3]); cries = collections.defaultdict(list)
    for f in z2.namelist():
        m2 = re.match(r'^(germaninjure|germandeath)/gen_([mno])_(pain|death)\d+\.wav$', f)
        if m2: cries[(m2.group(2), m2.group(3))].append('dodsp/voice/' + f)
    lines += ['// Pain and death cries (3 speakers m/n/o). Wave files go in sound/dodsp/voice/germaninjure/ and germandeath/', '']
    for (spk, kind) in sorted(cries):
        lines += [f'"GermanVoice.{spk}.{kind}"', '{', '\t"channel"\t\t"CHAN_VOICE"', '\t"volume"\t\t"1.0"',
                  '\t"soundlevel"\t"SNDLVL_85dB"', '\t"pitch"\t\t\t"PITCH_NORM"', '\t"rndwave"', '\t{']
        lines += [f'\t\t"wave"\t"{w}"' for w in sorted(cries[(spk, kind)])]
        lines += ['\t}', '}', '']
        n += 1
open(out, 'w', newline='\r\n').write('\n'.join(lines))
print(n, 'entries;', len(bases), 'categories')
for b in sorted(bases): print(' ', b, sorted(bases[b]))
