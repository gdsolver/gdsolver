#!/usr/bin/env python3
"""check_state_serial.py: the field table of the canonical serializer (dp/src/dp/state_serial.hpp DP_STATE_FIELDS) against the members of struct State (dp/src/dp/state.hpp).

Run it after touching struct State. A member without a row, a row without a member, a different order, or a wrong kind (pointer / float / skipped) is an error (exit 1). The C++ side has a static_assert on the
sum of the members' sizes plus the padding, which catches a size change; this catches a rename or a swap of equal-sized members. ASCII only.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE = os.path.join(ROOT, 'dp', 'src', 'dp', 'state.hpp')
SERIAL = os.path.join(ROOT, 'dp', 'src', 'dp', 'state_serial.hpp')


def split_top(s):
    out, d, cur = [], 0, ''
    for ch in s:
        if ch in '{(<[':
            d += 1
        if ch in '})>]':
            d -= 1
        if ch == ',' and d == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def members():
    src = open(STATE, encoding='utf-8', errors='replace').read()
    i = src.index('struct State {')
    d, j = 0, i
    for k in range(i, len(src)):
        if src[k] == '{':
            d += 1
        elif src[k] == '}':
            d -= 1
            if d == 0:
                j = k
                break
    body = src[i:j + 1]
    body = re.sub(r'//[^\n]*', '', body)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    d, stmt, out = 0, '', []
    for ch in body[body.index('{') + 1:-1]:
        if ch == '{':
            d += 1
            stmt += ch
        elif ch == '}':
            d -= 1
            stmt += ch
        elif ch == ';' and d == 0:
            out.append(' '.join(stmt.split()))
            stmt = ''
        else:
            stmt += ch
    res = []
    for s in out:
        if not s:
            continue
        m = re.match(r'^(const struct SpdSpillNode\*|const Obj\*|dp::gdtrig::TogBits|TouchMask|GravLatch|float|uint8_t|uint16_t|uint32_t|uint64_t|int16_t|int32_t)\s+(.*)$', s)
        if not m:
            print('check_state_serial: cannot read the member declaration: %s' % s)
            sys.exit(1)
        typ = m.group(1)
        for dcl in split_top(m.group(2)):
            nm = re.match(r'^([A-Za-z_]\w*)', dcl).group(1)
            if typ == 'const Obj*':
                k = 'P'
            elif typ == 'const struct SpdSpillNode*':
                k = 'S'
            elif nm in ('parent', 'evgate'):
                k = 'K'
            elif nm in ('slRef', 'slRef2'):
                k = 'L'
            elif typ == 'float':
                k = 'f'
            else:
                k = 'i'
            res.append((k, nm))
    return res


def table():
    src = open(SERIAL, encoding='utf-8').read()
    return [(m.group(1), m.group(2)) for m in re.finditer(r"X\('(.)',\s*(\w+)\)", src)]


def families_check(names):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import state_families
    fam = state_families.family_of()
    ok = True
    for n in names:
        if n not in fam:
            print('check_state_serial: member %s has no family in tools/state_families.py' % n)
            ok = False
    for n in sorted(set(fam) - set(names)):
        print('check_state_serial: tools/state_families.py names %s, which is not a member of State' % n)
        ok = False
    return ok


def main():
    a, b = members(), table()
    ok = True
    if len(a) != len(b):
        print('check_state_serial: State has %d members, the table has %d rows' % (len(a), len(b)))
        ok = False
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            print('check_state_serial: row %d: State has %s %s, the table has %s %s' % (i, a[i][0], a[i][1], b[i][0], b[i][1]))
            ok = False
            break
    sa, sb = {n for _, n in a}, {n for _, n in b}
    for n in sorted(sa - sb):
        print('check_state_serial: member %s has no row' % n)
    for n in sorted(sb - sa):
        print('check_state_serial: row %s has no member' % n)
    if sa != sb:
        ok = False
    if not families_check([n for _, n in a]):
        ok = False
    if ok:
        print('check_state_serial: ok, %d members, every one in a family' % len(a))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())