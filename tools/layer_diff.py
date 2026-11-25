#!/usr/bin/env python3
"""layer_diff.py A.csv B.csv [--all]: compare two --layerdigest files layer by layer and name the first layer whose PROJECTION differs.

A file is `# layerdigest v1: rung,t,n,sum,xor,ymin,ymax` and one line per layer the search finished (leveldp --layerdigest FILE). Two builds that agree layer by layer write the same lines.
WHAT AN "IDENTICAL" RESULT MEANS (audit AUD-856): v1 is a hash of a PROJECTION of each state (the dedupe key, y, vy, xAbs, bandAnim, tight, portSeen), summed over the layer. Identical files say the layers' projections agree with
probability 1 - 2^-64 per difference -- they do NOT say the states are bit-for-bit equal: states that differ only in y2 / vy2 inside one bin, in the lineage, or in a pointer have the same fingerprint.

layer_diff.py A.csv B.csv: two --layerdigest2 files (header `# layerdigest v2: rung,t,n,hashA,hashB`, leveldp --layerdigest2 FILE) are compared the same way, on a 128-bit digest of EVERY field of every state
(dp/src/dp/state_serial.hpp). Equal lines mean the layers hold the same SET of states bit for bit (the order does not matter; the lineage `parent`, the padding and the addresses of pointers are not in it).

layer_diff.py --bytes A.bin B.bin FIELDS.json: two layer dumps (leveldp --layerdigest2 FILE --layerbytes2 T writes FILE.t<T>.r<rung>.bin and FILE.fields.json) are compared state by state: the states only one
side has are listed, and for each the fields in which it differs from the closest state of the other side.

layer_diff.py --families A.bin B.bin FIELDS.json (or two directories of dumps with the same file names): the same pairing, tallied by family (tools/state_families.py) and by field: which family a change reaches.

Layers are matched by (rung, t); a file may hold several rungs (ladder rungs, resimulations: the layer clock went back). Exit code 0 = identical, 1 = a difference, 2 = unreadable.
ASCII only.
"""
import collections
import json
import os
import struct
import sys


def is_v2(path):
    try:
        with open(path, encoding='utf-8') as f:
            return f.readline().startswith('# layerdigest v2')
    except OSError:
        return False


def read(path):
    rows = []
    try:
        with open(path, encoding='utf-8') as f:
            for ln in f:
                ln = ln.strip()
                if not ln or ln.startswith('#'):
                    continue
                p = ln.split(',')
                if len(p) < 5:
                    raise ValueError('short line: %r' % ln)
                rows.append((int(p[0]), int(p[1]), int(p[2]), p[3], p[4], p[5:]))
    except (OSError, ValueError) as e:
        print('layer_diff: cannot read %s: %s' % (path, e), file=sys.stderr)
        sys.exit(2)
    return rows


def read_bin(path):
    with open(path, 'rb') as f:
        d = f.read()
    magic, t, n = struct.unpack_from('<IqI', d, 0)
    if magic != 0x324c4447:
        raise ValueError('not a layerdigest2 dump: %s' % path)
    off, recs = 16, []
    for _ in range(n):
        (ln,) = struct.unpack_from('<I', d, off)
        off += 4
        recs.append(d[off:off + ln])
        off += ln
    return t, recs


def decode(rec, fields):
    out, off = [], 0
    for fl in fields:
        if fl['kind'] == 'S':
            (c,) = struct.unpack_from('<I', rec, off)
            ln = 4 + 4 * c
        else:
            ln = fl['bytes']
        out.append(rec[off:off + ln])
        off += ln
    return out


def bytes_mode(pa, pb, pj):
    try:
        fields = [f for f in json.load(open(pj))['fields'] if f['kind'] != 'K']
        ta, ra = read_bin(pa)
        tb, rb = read_bin(pb)
    except (OSError, ValueError, struct.error, KeyError) as e:
        print('layer_diff: cannot read the dumps: %s' % e, file=sys.stderr)
        return 2
    print('A: t=%d, %d states; B: t=%d, %d states' % (ta, len(ra), tb, len(rb)))
    sa, sb = set(ra), set(rb)
    oa, ob = [r for r in ra if r not in sb], [r for r in rb if r not in sa]
    print('states only in A: %d, only in B: %d' % (len(oa), len(ob)))
    for r in oa[:5]:
        da = decode(r, fields)
        best, bd = None, None
        for q in ob:
            dq = decode(q, fields)
            nd = sum(1 for x, y in zip(da, dq) if x != y)
            if bd is None or nd < bd:
                best, bd = dq, nd
        if best is None:
            print('  a state of A has no counterpart in B')
            continue
        print('  closest state of B differs in %d field(s):' % bd)
        for fl, x, y in zip(fields, da, best):
            if x != y:
                print('    %s: A %s  B %s' % (fl['name'], x.hex(), y.hex()))
    return 0 if not oa and not ob else 1


def family_table(pa, pb, pj, limit=400):
    """Per-family tally of the fields in which the states only one side has differ from their closest state of the other side (one layer dump each)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import state_families
    fam = state_families.family_of()
    fields = [f for f in json.load(open(pj))['fields'] if f['kind'] != 'K']
    ta, ra = read_bin(pa)
    tb, rb = read_bin(pb)
    sa, sb = set(ra), set(rb)
    oa, ob = [r for r in ra if r not in sb][:limit], [r for r in rb if r not in sa][:limit]
    dob = [decode(q, fields) for q in ob]
    per_field, per_family, pairs = collections.Counter(), collections.Counter(), 0
    for r in oa:
        da = decode(r, fields)
        best, bd = None, None
        for dq in dob:
            nd = sum(1 for x, y in zip(da, dq) if x != y)
            if bd is None or nd < bd:
                best, bd = dq, nd
        if best is None:
            continue
        pairs += 1
        for fl, x, y in zip(fields, da, best):
            if x != y:
                per_field[fl['name']] += 1
                per_family[fam.get(fl['name'], 'other')] += 1
    return ta, tb, len(oa), len(ob), pairs, per_family, per_field


def families_mode(args):
    """layer_diff.py --families A.bin B.bin FIELDS.json | A_DIR B_DIR FIELDS.json: the differing fields of the closest-state pairs, tallied by family (tools/state_families.py) and by field."""
    pa, pb, pj = args
    if os.path.isdir(pa) and os.path.isdir(pb):
        names = sorted(n for n in os.listdir(pa) if n.endswith('.bin') and os.path.exists(os.path.join(pb, n)))
        pairs = [(os.path.join(pa, n), os.path.join(pb, n)) for n in names]
    else:
        pairs = [(pa, pb)]
    fam_tot, fld_tot, layers_diff, states = collections.Counter(), collections.Counter(), 0, 0
    for x, y in pairs:
        try:
            ta, tb, na, nb, np_, pf, pfl = family_table(x, y, pj)
        except (OSError, ValueError, struct.error, KeyError) as e:
            print('layer_diff: cannot read %s / %s: %s' % (x, y, e), file=sys.stderr)
            return 2
        if na or nb:
            layers_diff += 1
        states += np_
        fam_tot.update(pf)
        fld_tot.update(pfl)
        print('t=%d: only in A %d, only in B %d; differing fields by family: %s' % (ta, na, nb, ', '.join('%s %d' % kv for kv in sorted(pf.items())) or 'none'))
    print('%d dump(s), %d differ, %d state pairs; by family: %s' % (len(pairs), layers_diff, states, ', '.join('%s %d' % kv for kv in sorted(fam_tot.items())) or 'none'))
    for name, n in fld_tot.most_common(25):
        print('  %-18s %d' % (name, n))
    return 0 if not layers_diff else 1

def main():
    if len(sys.argv) >= 2 and sys.argv[1] == '--families':
        if len(sys.argv) != 5:
            print(__doc__)
            return 2
        return families_mode(sys.argv[2:5])
    if len(sys.argv) >= 2 and sys.argv[1] == '--bytes':
        if len(sys.argv) != 5:
            print(__doc__)
            return 2
        return bytes_mode(sys.argv[2], sys.argv[3], sys.argv[4])
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    show_all = '--all' in sys.argv
    if len(args) != 2:
        print(__doc__)
        return 2
    v2a, v2b = is_v2(args[0]), is_v2(args[1])
    if v2a != v2b:
        print('layer_diff: one file is layerdigest v2 and the other is not', file=sys.stderr)
        return 2
    v2 = v2a
    ha, hb = ('hashA', 'hashB') if v2 else ('sum', 'xor')
    a, b = read(args[0]), read(args[1])
    n = min(len(a), len(b))
    bad = 0
    first = None
    for i in range(n):
        if a[i][:5] != b[i][:5]:
            bad += 1
            if first is None:
                first = i
            if show_all:
                print('layer %d (rung %d t=%d): n %d vs %d, %s %s vs %s, %s %s vs %s' % (i, a[i][0], a[i][1], a[i][2], b[i][2], ha, a[i][3], b[i][3], hb, a[i][4], b[i][4]))
    note = 'v2: every field of every state, as a set' if v2 else 'v1 compares a projection of the states, not the states: see the docstring'
    print('%d layers in A, %d in B; compared %d; %d differ (%s)' % (len(a), len(b), n, bad, note))
    if first is not None:
        x, y = a[first], b[first]
        print('first difference: line %d, rung %d/%d, t=%d/%d' % (first, x[0], y[0], x[1], y[1]))
        print('  A: n=%d %s=%s %s=%s y=[%s]' % (x[2], ha, x[3], hb, x[4], ','.join(x[5])))
        print('  B: n=%d %s=%s %s=%s y=[%s]' % (y[2], ha, y[3], hb, y[4], ','.join(y[5])))
        if x[2] != y[2]:
            print('  the layers hold %d and %d states' % (x[2], y[2]))
        else:
            print('  the same number of states with a different set')
        if v2:
            print('  to name the field: re-run both with --layerbytes2 %d, then layer_diff.py --bytes A.t%d.r%d.bin B.t%d.r%d.bin FILE.fields.json' % (x[1], x[1], x[0], y[1], y[0]))
    if len(a) != len(b):
        print('the files end at different layers (%s)' % ('A is longer' if len(a) > len(b) else 'B is longer'))
    return 0 if (bad == 0 and len(a) == len(b)) else 1


if __name__ == '__main__':
    sys.exit(main())
