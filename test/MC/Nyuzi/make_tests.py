#
# This automatically generates assembler-tests.s and disassembler-tests.s
# with all instruction encoding types.  To use,
#
#   python encode_tests.py
#

import random
import sys


# Register arithmetic
def encode_r_instruction(fmt, opcode, dest, src1, src2, mask):
    return ((6 << 29) | (fmt << 26) | (opcode << 20) | (src2 << 15)
            | (mask << 10) | (dest << 5) | src1)


# Immediate arithmetic
def encode_i_instruction(fmt, opcode, dest, src1, imm, mask):
    return ((fmt << 29) | (opcode << 24) | ((imm & 0x1ff) << 15)
            | (mask << 10) | (dest << 5) | src1)


# Immediate arithmetic masked
def encode_im_instruction(fmt, opcode, dest, src1, imm):
    return ((fmt << 29) | (opcode << 24) | ((imm & 0x3fff) << 10)
            | (dest << 5) | src1)


# Memory
def encode_m_instruction(isLoad, op, srcDest, ptr, offs, mask):
    return ((1 << 31) | (isLoad << 29) | (op << 25) | (offs << 15)
            | (mask << 10) | (srcDest << 5) | ptr)


# Memory masked
def encode_mm_instruction(isLoad, op, srcDest, ptr, offs):
    return ((1 << 31) | (isLoad << 29) | (op << 25) | (offs << 10)
            | (srcDest << 5) | ptr)

# Cache control
def encode_c_instruction(op, reg):
    return 0xe0000000 | (op << 25) | reg


# Cache control TLB
def encode_cprime_instruction(op, reg, physReg):
    return 0xe0000000 | (op << 25) | (physReg << 5) | reg


def encode_text_encoding(x, sep):
    str = ''
    for y in range(4):
        if y != 0:
            str += sep

        str += '0x%02x' % (x & 0xff)
        x >>= 8

    return str


#
# This generates both the assembler and disassembler test
# for the same instruction
#
def write_test_case(string, encoding):
    global disasm_fp
    global asm_fp

    asm_fp.write(string + (' ' * (32 - len(string))) + ' # CHECK: ' +
                 encode_text_encoding(encoding, ',') + '\n')
    disasm_fp.write(encode_text_encoding(encoding, ' ') +
                    ' # CHECK: ' + string + '\n')

nextreg = 0


def getnextreg():
    global nextreg

    nextreg += 1
    if nextreg > 27:
        nextreg = 0

    return nextreg

# Setup
disasm_fp = open('disassembler-tests.s', 'w')
asm_fp = open('assembler-tests.s', 'w')

asm_fp.write('# This file auto-generated by ' +
             sys.argv[0] + '. Do not edit.\n')
asm_fp.write('# RUN: llvm-mc -arch=nyuzi -show-encoding %s | FileCheck %s\n')

disasm_fp.write('# This file auto-generated by ' +
                sys.argv[0] + '. Do not edit.\n')
disasm_fp.write('# RUN: llvm-mc -arch=nyuzi -disassemble %s | FileCheck %s\n')

##############################################################
# Test cases
##############################################################

binary_ops = [
    (0, 'or'),
    (1, 'and'),
    (3, 'xor'),
    (5, 'add_i'),
    (6, 'sub_i'),
    (7, 'mull_i'),
    (8, 'mulh_u'),
    (9, 'ashr'),
    (10, 'shr'),
    (11, 'shl'),
    (0x20, 'add_f'),
    (0x21, 'sub_f'),
    (0x22, 'mul_f'),
    (0x1f, 'mulh_i'),
]

r_instruction_types = [
    ('s', 's', 's', 0, False),
    ('v', 'v', 's', 1, False),
    ('v', 'v', 's', 2, True),
    ('v', 'v', 'v', 4, False),
    ('v', 'v', 'v', 5, True)
]

i_instruction_types = [
    ('s', 0, False),
    ('v', 1, False),
    ('v', 3, True),
]

for opcode, mnemonic in binary_ops:
    for dregt, s1regt, s2regt, fmt, is_masked in r_instruction_types:
        dreg = getnextreg()
        s1reg = getnextreg()
        s2reg = getnextreg()
        mreg = getnextreg()
        encoded = encode_r_instruction(
            fmt, opcode, dreg, s1reg, s2reg, mreg if is_masked else 0)
        asm_str = mnemonic + ('_mask ' if is_masked else ' ') + \
            dregt + str(dreg) + ', '
        if is_masked:
            asm_str += 's' + str(mreg) + ', '

        asm_str += s1regt + str(s1reg) + ', ' + s2regt + str(s2reg)
        write_test_case(asm_str, encoded)

    if mnemonic[-2:] == '_f':
        continue  # Can't do immediate for FP instructions

    for regt, fmt, is_masked in i_instruction_types:
        dreg = getnextreg()
        sreg = getnextreg()
        mreg = getnextreg()
        imm = random.randint(-128, 127)
        if is_masked:
            encoded = encode_i_instruction(fmt, opcode, dreg, sreg, imm, mreg)
        else:
            encoded = encode_im_instruction(fmt, opcode, dreg, sreg, imm)

        asm_str = mnemonic + ('_mask ' if is_masked else ' ') + \
            regt + str(dreg) + ', '
        if is_masked:
            asm_str += 's' + str(mreg) + ', '

        asm_str += regt + str(sreg) + ', ' + str(imm)
        write_test_case(asm_str, encoded)

unary_ops = [
    (12, 'clz'),
    (14, 'ctz'),
    (0xf, 'move'),
    (0x1c, 'reciprocal'),
]

# These unary ops do not support all forms
#	(0x1b, 'ftoi'),
#	(0x1d, 'sext_8'),
#	(0x1e, 'sext_16'),
#	(0x2a, 'itof')

unary_op_types = [
    (0, 's', 's', False),
    (1, 'v', 's', False),
    (2, 'v', 's', True),
    (4, 'v', 'v', False),
    (5, 'v', 'v', True)

]

nextreg = 0
for opcode, mnemonic in unary_ops:
    rega = getnextreg()
    regb = getnextreg()
    regm = getnextreg()

    for fmt, destregt, src1regt, is_masked in unary_op_types:
        if is_masked:
            write_test_case(mnemonic + '_mask' + ' ' + destregt + str(rega) + ', ' +
                                's' + str(regm) + ', ' + src1regt + str(regb),
                               encode_r_instruction(fmt, opcode, rega, 0, regb, regm))
        else:
            write_test_case(mnemonic + ' ' + destregt + str(rega) + ', ' +
                                src1regt + str(regb),
                               encode_r_instruction(fmt, opcode, rega, 0, regb, 0))

nextreg = 0

# XXX Source register needs to be set to 1 to pass. Investigate in
# LLVM assembler.
write_test_case('move s1, 72', encode_im_instruction(0, 0xf, 1, 1, 72))
write_test_case('move v1, 72', encode_im_instruction(1, 0xf, 1, 1, 72))

write_test_case('shuffle v1, v2, v3',
                   encode_r_instruction(4, 0xd, 1, 2, 3, 0))
write_test_case('shuffle_mask v1, s4, v2, v3',
                   encode_r_instruction(5, 0xd, 1, 2, 3, 4))

write_test_case('getlane s4, v5, s6',
                   encode_r_instruction(1, 0x1a, 4, 5, 6, 0))
write_test_case('getlane s4, v5, 7',
                   encode_im_instruction(1, 0x1a, 4, 5, 7))

# XXX HACK: These instructions should support all forms, but this is here
# in the interim
write_test_case('sext_8 s8, s9', encode_r_instruction(0, 0x1d, 8, 0, 9, 0))
write_test_case('sext_16 s8, s9', encode_r_instruction(0, 0x1e, 8, 0, 9, 0))
write_test_case('itof s8, s9', encode_r_instruction(0, 0x2a, 8, 0, 9, 0))
write_test_case('ftoi s8, s9', encode_r_instruction(0, 0x1b, 8, 0, 9, 0))
write_test_case('itof v8, v9', encode_r_instruction(4, 0x2a, 8, 0, 9, 0))
write_test_case('ftoi v8, v9', encode_r_instruction(4, 0x1b, 8, 0, 9, 0))
write_test_case('itof v8, s9', encode_r_instruction(1, 0x2a, 8, 0, 9, 0))
write_test_case('ftoi v8, s9', encode_r_instruction(1, 0x1b, 8, 0, 9, 0))

write_test_case('nop', encode_i_instruction(0, 0, 0, 0, 0, 0))

#
# Comparisons
#

compare_ops = [
    (0x10, 'eq_i'),
    (0x11, 'ne_i'),
    (0x12, 'gt_i'),
    (0x13, 'ge_i'),
    (0x14, 'lt_i'),
    (0x15, 'le_i'),
    (0x16, 'gt_u'),
    (0x17, 'ge_u'),
    (0x18, 'lt_u'),
    (0x19, 'le_u'),
    (0x2c, 'gt_f'),
    (0x2d, 'ge_f'),
    (0x2e, 'lt_f'),
    (0x2f, 'le_f')
]

nextreg = 0
for opcode, mnemonic in compare_ops:
    rega = getnextreg()
    regb = getnextreg()
    regc = getnextreg()

    write_test_case('cmp' + mnemonic + ' s' + str(rega) + ', s'
                       + str(regb) + ', s' + str(regc),
                       encode_r_instruction(0, opcode, rega, regb, regc, 0))

    write_test_case('cmp' + mnemonic + ' s' + str(rega) + ', v' + str(regb)
                       + ', s' + str(regc),
                       encode_r_instruction(1, opcode, rega, regb, regc, 0))

    write_test_case('cmp' + mnemonic + ' s' + str(rega) + ', v' + str(regb)
                       + ', v' + str(regc),
                       encode_r_instruction(4, opcode, rega, regb, regc, 0))

    if mnemonic[-2:] == '_f':
        continue  # Can't do immediate for FP instructions

    imm = random.randint(0, 255)
    write_test_case('cmp' + mnemonic + ' s' + str(rega) + ', s' + str(regb)
                       + ', ' + str(imm),
                       encode_im_instruction(0, opcode, rega, regb, imm))

    write_test_case('cmp' + mnemonic + ' s' + str(rega) + ', v' + str(regb)
                       + ', ' + str(imm),
                       encode_im_instruction(1, opcode, rega, regb, imm))


#
# Scalar load/stores
#

scalarMemFormats = [
    ('load_u8', 0, 1),
    ('load_s8', 1, 1),
    ('load_u16', 2, 1),
    ('load_s16', 3, 1),
    ('load_32', 4, 1),
    ('load_sync', 5, 1),
    ('store_8', 1, 0),
    ('store_16', 3, 0),
    ('store_32', 4, 0),
    ('store_sync', 5, 0)
]

nextreg = 0
for stem, fmt, isLoad in scalarMemFormats:
    rega = getnextreg()
    regb = getnextreg()
    offs = getnextreg()
    write_test_case(stem + ' s' + str(rega) + ', (s' + str(regb) + ')',
                       encode_mm_instruction(isLoad, fmt, rega, regb, 0))  # No offset
    write_test_case(stem + ' s' + str(rega) + ', ' + str(offs) + '(s' + str(regb) + ')',
                       encode_mm_instruction(isLoad, fmt, rega, regb, offs))  # offset

#
# Vector load/stores
#

vector_mem_fmts = [
    ('v', 'v', 's', 7),
    ('gath', 'scat', 'v', 0xd)
]

nextreg = 0
for load_suffix, store_suffix, ptr_type, op in vector_mem_fmts:
    rega = getnextreg()
    regb = getnextreg()
    mask = getnextreg()
    offs = random.randint(0, 128) * 4

    load_stem = 'load_' + load_suffix

    # Offset
    write_test_case(load_stem + ' v' + str(rega) + ', ' + str(offs) + '('
                       + ptr_type + str(regb) + ')',
                       encode_mm_instruction(1, op, rega, regb, offs))
    write_test_case(load_stem + '_mask v' + str(rega) + ', s' + str(mask)
                       + ', ' + str(offs) + '(' + ptr_type + str(regb) + ')',
                       encode_m_instruction(1, op + 1, rega, regb, offs, mask))

    # No offset
    write_test_case(load_stem + ' v' + str(rega) + ', (' + ptr_type
                       + str(regb) + ')',
                       encode_mm_instruction(1, op, rega, regb, 0))
    write_test_case(load_stem + '_mask v' + str(rega) + ', s' + str(mask)
                       + ', (' + ptr_type + str(regb) + ')',
                       encode_m_instruction(1, op + 1, rega, regb, 0, mask))

    storeStem = 'store_' + store_suffix

    # Offset
    write_test_case(storeStem + ' v' + str(rega) + ', ' + str(offs)
                       + '(' + ptr_type + str(regb) + ')',
                       encode_mm_instruction(0, op, rega, regb, offs))
    write_test_case(storeStem + '_mask v' + str(rega) + ', s' + str(mask)
                       + ', ' + str(offs) + '(' + ptr_type + str(regb) + ')',
                       encode_m_instruction(0, op + 1, rega, regb, offs, mask))

    # No offset
    write_test_case(storeStem + ' v' + str(rega) + ', (' + ptr_type
                       + str(regb) + ')',
                       encode_mm_instruction(0, op, rega, regb, 0))
    write_test_case(storeStem + '_mask v' + str(rega) + ', s' + str(mask)
                       + ', (' + ptr_type + str(regb) + ')',
                       encode_m_instruction(0, op + 1, rega, regb, 0, mask))

# Control register
nextreg = 0
write_test_case('getcr s7, 9', encode_mm_instruction(1, 6, 7, 9, 0))
write_test_case('setcr s11, 13', encode_mm_instruction(0, 6, 11, 13, 0))

# Cache control
write_test_case('dflush s7', encode_c_instruction(2, 7))
write_test_case('membar', encode_c_instruction(4, 0))
write_test_case('dinvalidate s9', encode_c_instruction(1, 9))
write_test_case('iinvalidate s11', encode_c_instruction(3, 11))
write_test_case('tlbinval s12', encode_c_instruction(5, 12))
write_test_case('tlbinvalall', encode_c_instruction(6, 0))
write_test_case('dtlbinsert s1, s2', encode_cprime_instruction(0, 1, 2))
write_test_case('itlbinsert s3, s4', encode_cprime_instruction(7, 3, 4))

# Special instructions
write_test_case('break', 0xc3e00000)
write_test_case('syscall', 0xc3f00000)

# Cleanup
disasm_fp.close()
asm_fp.close()
