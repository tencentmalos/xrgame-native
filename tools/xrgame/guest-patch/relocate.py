# SPDX-License-Identifier: GPL-2.0-or-later
"""Function-entry trampoline relocation, computed when a package is built.

Port of shadPS4 src/core/host_runtime/guest_patch_relocator.cpp (Zydis) to capstone.
The in-game installer has no disassembler: the builder decodes the stolen
instructions here and emits a template (bytes plus fixups in module offsets) that
the installer finishes once it knows the module base and the trampoline address.

Complete instructions, RIP-relative memory, rel32 calls/jumps and short/near Jcc are
relocated; PC-sensitive or control forms that cannot be moved reliably are refused.
"""
import struct

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone import x86_const as X

# Copying these would silently change observable semantics (IP, interrupts,
# transactions, non-local control), as in the shadPS4 relocator.
REFUSED = {'loop', 'loope', 'loopne', 'jrcxz', 'jecxz', 'ret', 'retf', 'iret', 'iretd', 'iretq',
           'int', 'int3', 'into', 'syscall', 'sysret', 'ud2', 'xbegin', 'xend', 'xabort',
           'fnstenv', 'fxsave', 'fxsave64', 'fstenv'}
LEGACY_PREFIXES = {0x66, 0x67, 0xf0, 0xf2, 0xf3, 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65}


class RelocationError(ValueError):
    pass


def _check(ok, why):
    if not ok:
        raise RelocationError(why)


def _encoding_byte(raw):
    """First byte after legacy prefixes and REX: 0x62 starts EVEX, 0x8f with mmmmm>=8 XOP."""
    i = 0
    while i < len(raw) and raw[i] in LEGACY_PREFIXES:
        i += 1
    if i < len(raw) and 0x40 <= raw[i] <= 0x4f:
        i += 1
    return raw[i:i + 2]


def relocate(code, source, minimum=5):
    """Template for the original-function trampoline of the window at module offset `source`.

    `code` holds the preimage starting at `source` (at least `minimum` bytes of whole
    instructions). Returns {stolen, code, fixups, map}: `code` is the trampoline with
    placeholders, each fixup fills one of them:
      {"at": n, "kind": "rel32", "end": e, "target": t}  int32 = base+t - (tramp+e)
      {"at": n, "kind": "abs64", "target": t}            uint64 = base+t
      {"at": n, "kind": "abs64_local", "target": o}      uint64 = tramp+o
    The last fixup jumps back to source+stolen.
    """
    _check(5 <= minimum <= 32 and len(code) <= 256, 'invalid trampoline window')
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    decoded = []
    offset = 0
    while offset < minimum:
        _check(offset < len(code), 'truncated trampoline preimage')
        insn = next(md.disasm(code[offset:], source + offset, 1), None)
        _check(insn is not None, 'invalid x86 instruction at +%d' % offset)
        raw = bytes(insn.bytes)
        lead = _encoding_byte(raw)
        _check(not lead.startswith(b'\x62'), 'EVEX encoding is refused')
        _check(not (len(lead) == 2 and lead[0] == 0x8f and (lead[1] & 0x1f) >= 8), 'XOP encoding is refused')
        _check(insn.mnemonic not in REFUSED,
               'unsupported PC/control-sensitive trampoline instruction: ' + insn.mnemonic)
        decoded.append((offset, insn))
        offset += insn.size
    stolen = offset

    # Output layout first: relative branches grow into absolute forms.
    kinds, out_offsets, out = [], [], 0
    for at, insn in decoded:
        kind = 'copy'
        if X.X86_GRP_BRANCH_RELATIVE in insn.groups:
            target = insn.operands[0].imm
            if insn.mnemonic == 'call':
                _check(target != source + at + insn.size, 'call/pop PC discovery is unsupported')
                kind, size = 'call', 16
            elif insn.mnemonic == 'jmp':
                kind, size = 'jmp', 14
            else:
                op = bytes(insn.opcode)
                _check(0x70 <= op[0] <= 0x7f or (op[0] == 0x0f and 0x80 <= op[1] <= 0x8f),
                       'unsupported relative instruction: ' + insn.mnemonic)
                kind, size = 'jcc', 16
        else:
            size = insn.size
        kinds.append(kind)
        out_offsets.append(out)
        out += size

    def target_of(address):
        """Module offset outside the window, else ('local', output offset)."""
        if not source <= address < source + stolen:
            return None
        for (at, _), o in zip(decoded, out_offsets):
            if source + at == address:
                return o
        raise RelocationError('branch enters middle of stolen instruction')

    result = bytearray()
    fixups = []

    def absolute(address):
        local = target_of(address)
        fixups.append({'at': len(result), 'kind': 'abs64' if local is None else 'abs64_local',
                       'target': address if local is None else local})
        result.extend(b'\0' * 8)

    mapping = []
    for (at, insn), kind, o in zip(decoded, kinds, out_offsets):
        mapping.append([source + at, o])
        assert len(result) == o
        if kind == 'jmp':
            result += b'\xff\x25\0\0\0\0'
            absolute(insn.operands[0].imm)
        elif kind == 'jcc':
            op = bytes(insn.opcode)
            cc = (op[0] if op[0] != 0x0f else op[1]) & 0xf
            result += bytes([0x70 | (cc ^ 1), 14]) + b'\xff\x25\0\0\0\0'
            absolute(insn.operands[0].imm)
        elif kind == 'call':
            # call [rip+2]; jmp +8; target. No scratch GPR, preserves AL and flags.
            result += b'\xff\x15\x02\0\0\0\xeb\x08'
            absolute(insn.operands[0].imm)
        else:
            raw = bytearray(insn.bytes)
            for operand in insn.operands:
                if operand.type != X.X86_OP_MEM:
                    continue
                _check(operand.mem.base != X.X86_REG_EIP, 'address-size EIP relocation unsupported')
                if operand.mem.base != X.X86_REG_RIP:
                    continue
                address = source + at + insn.size + operand.mem.disp
                _check(not source <= address < source + stolen, 'RIP-relative data aliases overwritten entry')
                _check(insn.disp_size == 4, 'RIP-relative operand without disp32')
                fixups.append({'at': o + insn.disp_offset, 'kind': 'rel32', 'end': o + insn.size,
                               'target': address})
                raw[insn.disp_offset:insn.disp_offset + 4] = b'\0' * 4
            result += raw
    result += b'\xff\x25\0\0\0\0'
    fixups.append({'at': len(result), 'kind': 'abs64', 'target': source + stolen})
    result += b'\0' * 8
    return {'stolen': stolen, 'code': bytes(result).hex(), 'fixups': fixups, 'map': mapping}


def apply(template, base, trampoline):
    """The installer's arithmetic, for tests: finished trampoline bytes."""
    code = bytearray(bytes.fromhex(template['code']))
    for f in template['fixups']:
        if f['kind'] == 'rel32':
            value = base + f['target'] - (trampoline + f['end'])
            _check(-2**31 <= value < 2**31, 'RIP-relative target outside rel32 range')
            code[f['at']:f['at'] + 4] = struct.pack('<i', value)
        elif f['kind'] == 'abs64':
            code[f['at']:f['at'] + 8] = struct.pack('<Q', base + f['target'])
        else:
            code[f['at']:f['at'] + 8] = struct.pack('<Q', trampoline + f['target'])
    return bytes(code)
