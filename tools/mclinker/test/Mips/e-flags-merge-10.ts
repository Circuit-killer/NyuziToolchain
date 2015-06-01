# RUN: yaml2obj -format=elf -docnum 1 %s > %t-32r2.o
# RUN: yaml2obj -format=elf -docnum 2 %s > %t-32r6.o
# RUN: not %MCLinker -mtriple=mipsel-unknown-linux -o %t.exe \
# RUN: %t-32r2.o %t-32r6.o 2>&1 | FileCheck %s

# CHECK: target arch 'mips32r2' is inconsist with the 'mips32r6' in e-flags-merge-10

---
FileHeader:
  Class:           ELFCLASS32
  Data:            ELFDATA2LSB
  Type:            ET_REL
  Machine:         EM_MIPS
  Flags:           [EF_MIPS_ABI_O32, EF_MIPS_ARCH_32R2]

Sections:
  - Name:          .text
    Type:          SHT_PROGBITS
    Flags:         [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign:  0x04
    Size:          0x04

# 32r6.o
---
FileHeader:
  Class:           ELFCLASS32
  Data:            ELFDATA2LSB
  Type:            ET_REL
  Machine:         EM_MIPS
  Flags:           [EF_MIPS_ABI_O32, EF_MIPS_ARCH_32R6]

Sections:
  - Name:          .text
    Type:          SHT_PROGBITS
    Flags:         [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign:  0x04
    Size:          0x04
...
