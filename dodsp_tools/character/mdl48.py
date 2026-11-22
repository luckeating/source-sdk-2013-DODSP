"""Reader for a compiled Source 2013 model (.mdl version 48 / 49): what a character QC sets, read back from the
compiled file - so a new body can copy the soldier's eyes, attachments, hitboxes, IK chains, includes and flags without
the owner's QC. Offsets from public/studio.h (studiohdr_t, mstudiobone_t, mstudioattachment_t, mstudiobbox_t, ...).

    python3 -I mdl48.py <model.mdl>          prints everything it reads"""
import struct, sys, numpy as np

def read(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'IDST', path
    I = lambda o: struct.unpack_from('<i', d, o)[0]
    F = lambda o, n=1: struct.unpack_from('<%df' % n, d, o)
    def cs(o):
        return d[o:d.index(b'\0', o)].decode('latin-1')
    h = dict(version=I(4), name=cs(12), eye=F(80, 3), illum=F(92, 3), hull=(F(104, 3), F(116, 3)), view=(F(128, 3), F(140, 3)),
             flags=I(152), mass=F(328)[0], contents=I(332), surfaceprop=cs(I(308)))
    bones = []
    nb, bi = I(156), I(160)
    for k in range(nb):
        o = bi + 216 * k
        M = np.array(F(o + 96, 12)).reshape(3, 4)           # poseToBone
        bones.append(dict(name=cs(o + I(o)), parent=I(o + 4), pos=F(o + 32, 3), quat=F(o + 44, 4), rot=F(o + 60, 3),
                          flags=I(o + 160), surfaceprop=cs(o + I(o + 176)), contents=I(o + 180), poseToBone=M))
    att = []
    for k in range(I(240)):
        o = I(244) + 92 * k
        att.append(dict(name=cs(o + I(o)), flags=I(o + 4), bone=I(o + 8), m=np.array(F(o + 12, 12)).reshape(3, 4)))
    hb = []
    for k in range(I(172)):
        o = I(176) + 12 * k; n, idx = I(o + 4), I(o + 8)
        boxes = []
        for j in range(n):
            b = o + idx + 68 * j
            boxes.append(dict(bone=I(b), group=I(b + 4), mn=F(b + 8, 3), mx=F(b + 20, 3), name=cs(b + I(b + 32)) if I(b + 32) else ''))
        hb.append(dict(name=cs(o + I(o)), boxes=boxes))
    inc = []
    for k in range(I(336)):
        o = I(340) + 8 * k; inc.append(cs(o + I(o + 4)))
    ik = []
    for k in range(I(284)):
        o = I(288) + 16 * k; links = []
        for j in range(I(o + 8)):
            l = o + I(o + 12) + 28 * j; links.append((I(l), F(l + 4, 3)))
        ik.append(dict(name=cs(o + I(o)), links=links))
    tex = [cs(I(208) + 64 * k + I(I(208) + 64 * k)) for k in range(I(204))]
    cd = [cs(I(I(216) + 4 * k)) for k in range(I(212))]
    body = []
    for k in range(I(232)):
        o = I(236) + 16 * k; models = []
        for j in range(I(o + 4)):
            m = o + I(o + 12) + 148 * j; models.append((cs(m), I(m + 76 + 4 * 2)))   # name, numvertices? (informational)
        body.append(dict(name=cs(o + I(o)), models=[mm[0] for mm in models]))
    seq = [cs(I(192) + 212 * k + I(I(192) + 212 * k + 4)) for k in range(I(188))]
    pose = [cs(I(304) + 20 * k + I(I(304) + 20 * k)) for k in range(I(300))]
    kv = d[I(312):I(312) + I(316)].decode('latin-1').rstrip('\0') if I(316) else ''
    return dict(h=h, bones=bones, attachments=att, hitboxsets=hb, includes=inc, ik=ik, textures=tex, cdtextures=cd,
                bodyparts=body, sequences=seq, poseparams=pose, keyvalues=kv, numskinfamilies=I(224), numskinref=I(220))

if __name__ == '__main__':
    m = read(sys.argv[1])
    for k, v in m['h'].items(): print(k, v)
    names = [b['name'] for b in m['bones']]
    print('bones', len(names)); [print('  %2d %-36s parent %-3d flags %08x %s' % (i, b['name'], b['parent'], b['flags'], b['surfaceprop'])) for i, b in enumerate(m['bones'])]
    print('attachments'); [print('  %-24s bone %-34s flags %d origin %s' % (a['name'], names[a['bone']], a['flags'], np.round(a['m'][:, 3], 3))) for a in m['attachments']]
    for s in m['hitboxsets']:
        print('hitbox set', s['name'], len(s['boxes']))
        for b in s['boxes']: print('  %-34s group %d %s %s %s' % (names[b['bone']], b['group'], np.round(b['mn'], 2), np.round(b['mx'], 2), b['name']))
    print('includes', m['includes']); print('ik', [(c['name'], [names[l[0]] for l in c['links']]) for c in m['ik']])
    print('textures', m['textures'], 'cd', m['cdtextures'], 'skins', m['numskinfamilies'], m['numskinref'])
    print('bodyparts', m['bodyparts']); print('sequences', len(m['sequences']), m['sequences'][:12]); print('poseparams', m['poseparams'])
    print('keyvalues', m['keyvalues'][:600])
