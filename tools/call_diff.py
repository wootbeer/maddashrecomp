#!/usr/bin/env python3
"""Run one call of the original x86 again in Unicorn and compare it with the
recompiled run.

    python tools/call_diff.py DIR ENTRY

DIR holds what src/guest/bink_diff.c saves (MDR_BINK_DIFF=n): bink_before.bin
and bink_after.bin (guest RAM, then the 0x80000000 window, 64 MB each) and
bink_regs.txt (esp and the return address at the call). ENTRY is the called
function (hex; 0x254590 for Bink's frame decoder). The XBE's code is in the
saved RAM, so nothing from the game is needed beyond the dump. Kernel thunks
(0xFE000000 on) return at once; a call that waits on another thread will not
run the same here. Prints the address ranges where the two runs differ.
Needs: pip install unicorn numpy.
"""
import sys
import numpy as np
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX

MB = 1 << 20


def main():
    d, entry = sys.argv[1], int(sys.argv[2], 16)
    before = open(d + '/bink_before.bin', 'rb').read()
    after = open(d + '/bink_after.bin', 'rb').read()
    regs = open(d + '/bink_regs.txt').read().split()
    esp, ret = int(regs[1], 16), int(regs[3], 16)

    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map(0, 64 * MB)
    mu.mem_write(0, before[:64 * MB])
    mu.mem_map(0x80000000, 64 * MB)
    mu.mem_write(0x80000000, before[64 * MB:])
    mu.mem_map(0xFE000000, 0x10000)
    mu.mem_write(0xFE000000, b'\xc3' * 0x10000)
    mu.reg_write(UC_X86_REG_ESP, esp)
    try:
        mu.emu_start(entry, ret, count=3_000_000_000)
    except UcError as e:
        print('emulation stopped:', e)
    print('eax', hex(mu.reg_read(UC_X86_REG_EAX)))

    emu = bytes(mu.mem_read(0, 64 * MB)) + bytes(mu.mem_read(0x80000000, 64 * MB))
    a = np.frombuffer(emu, np.uint8)
    b = np.frombuffer(after, np.uint8)
    diff = np.nonzero(a != b)[0]
    runs = []
    for i in diff:
        if runs and i - runs[-1][1] <= 16:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    print(len(diff), 'bytes differ, in', len(runs), 'runs')
    for lo, hi in runs[:60]:
        va = lo if lo < 64 * MB else 0x80000000 + lo - 64 * MB
        print('%08x %5d  emu %s  ours %s' % (va, hi - lo + 1, bytes(a[lo:lo + 16]).hex(), bytes(b[lo:lo + 16]).hex()))


if __name__ == '__main__':
    main()
