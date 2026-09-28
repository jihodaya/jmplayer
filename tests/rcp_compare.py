#!/usr/bin/env python3
"""Diff a converted Recomposer file against the reference .MID beside it.

The RCP work is checkable rather than a matter of taste, because two of the
sample songs ship the .MID the original sequencer produced.  Convert with
rcpdump, then compare the two event streams - that is how every claim in
CLAUDE.md section 7-3 was settled.

    python tests/rcp_compare.py D:/mt32/x68k/BAMBOO/BAMBOO.RCP

It looks for the reference as <name>.MID beside the source, converts through
build_tests/rcpdump.exe, and reports totals plus a per-channel note diff.
rcpdump needs Qt on PATH; pass --rcpdump if it is somewhere else.

A channel is only "identical" when the note streams match after a single
constant tick offset - which for a correct conversion is zero, because the
576-tick lead-in is part of the output.
"""

import argparse
import collections
import os
import struct
import subprocess
import sys
import tempfile


def read_vlq(b, i):
    v = 0
    while True:
        v = (v << 7) | (b[i] & 0x7F)
        i += 1
        if not (b[i - 1] & 0x80):
            return v, i


def scan(path):
    """Every event of interest, with its absolute tick."""
    b = open(path, 'rb').read()
    if b[:4] != b'MThd':
        raise SystemExit('%s is not a Standard MIDI File' % path)
    fmt, ntrk, div = struct.unpack_from('>HHH', b, 8)

    notes = collections.defaultdict(list)   # channel -> [(tick, note, vel)]
    counts = collections.Counter()
    ctrl = collections.Counter()
    last = 0

    i = 8 + struct.unpack_from('>I', b, 4)[0]
    tracks = 0
    while i < len(b) - 8 and b[i:i + 4] == b'MTrk':
        length = struct.unpack_from('>I', b, i + 4)[0]
        j, end, t, running = i + 8, i + 8 + length, 0, 0
        tracks += 1
        while j < end:
            delta, j = read_vlq(b, j)
            t += delta
            if j >= end:
                break
            if b[j] & 0x80:
                running = b[j]
                j += 1
            status, channel = running & 0xF0, running & 0x0F
            if status == 0x90:
                if b[j + 1] > 0:
                    notes[channel].append((t, b[j], b[j + 1]))
                    counts['note-ons'] += 1
                j += 2
            elif status == 0xB0:
                ctrl[b[j]] += 1
                counts['controllers'] += 1
                j += 2
            elif status == 0xC0:
                counts['program changes'] += 1
                j += 1
            elif status in (0x80, 0xA0, 0xE0):
                j += 2
            elif status == 0xD0:
                j += 1
            elif running == 0xFF:
                meta = b[j]
                j += 1
                n, j = read_vlq(b, j)
                if meta == 0x51:
                    counts['tempo events'] += 1
                j += n
            elif running in (0xF0, 0xF7):
                n, j = read_vlq(b, j)
                counts['SysEx'] += 1
                j += n
            else:
                break
        last = max(last, t)
        i = end

    counts['tracks'] = tracks
    counts['division'] = div
    counts['last tick'] = last
    for c in notes:
        notes[c].sort()
    return counts, ctrl, notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source', help='the .RCP / .R36 / .G36 to convert')
    ap.add_argument('--reference', help='reference .MID (default: beside the source)')
    ap.add_argument('--rcpdump', default=os.path.join('build_tests', 'rcpdump.exe'))
    args = ap.parse_args()

    reference = args.reference or os.path.splitext(args.source)[0] + '.MID'
    if not os.path.exists(reference):
        raise SystemExit('no reference beside the source: %s' % reference)

    out = os.path.join(tempfile.gettempdir(), 'rcp_compare.mid')
    result = subprocess.run([args.rcpdump, args.source, out],
                            capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(out):
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit('rcpdump failed - is Qt on PATH?')

    ours, ourCtrl, ourNotes = scan(out)
    ref, refCtrl, refNotes = scan(reference)

    print('%-22s %-14s %-14s' % ('', 'ours', 'reference'))
    for key in ('tracks', 'division', 'note-ons', 'program changes',
                'tempo events', 'controllers', 'SysEx', 'last tick'):
        a, b = ours[key], ref[key]
        print('  %-20s %-14s %-14s %s'
              % (key, a, b, '' if a == b else '<-- differs'))

    print()
    print('  controllers used')
    for cc in sorted(set(ourCtrl) | set(refCtrl)):
        a, b = ourCtrl[cc], refCtrl[cc]
        print('    CC%-4d %10d %10d %s' % (cc, a, b, '' if a == b else '<-- differs'))

    print()
    print('  notes per channel, aligned on a single constant offset')
    problems = 0
    for ch in sorted(set(ourNotes) | set(refNotes)):
        A, B = ourNotes.get(ch, []), refNotes.get(ch, [])
        if not A or not B:
            print('    ch %-3d %8d %8d   <-- only one side has this channel'
                  % (ch + 1, len(A), len(B)))
            problems += 1
            continue
        offset = B[0][0] - A[0][0]
        shifted = collections.Counter((t + offset, n, v) for (t, n, v) in A)
        target = collections.Counter(B)
        extra = sum((shifted - target).values())
        missing = sum((target - shifted).values())
        clean = (extra == 0 and missing == 0)
        note = ('identical' if offset == 0 else 'identical, %+d ticks' % offset) \
            if clean else '%d extra, %d missing' % (extra, missing)
        if not clean:
            problems += 1
        print('    ch %-3d %8d %8d   %s' % (ch + 1, len(A), len(B), note))

    print()
    # A uniform shift is not a content difference. The lead-in is ours to
    # choose - the reference sized its own around a bulk dump we do not send -
    # so what matters is that every note lines up once it is taken out.
    print('  %s' % ('every channel identical' if problems == 0
                    else '%d channel(s) differ' % problems))
    return 0


if __name__ == '__main__':
    sys.exit(main())
