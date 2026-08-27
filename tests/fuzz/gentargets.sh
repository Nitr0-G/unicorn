#!/bin/sh
set -eu

generate()
{
    arch=$1
    mode=$2
    output=$3

    sed -e "s/^#define UC_FUZZ_ARCH .*/#define UC_FUZZ_ARCH ${arch}/" \
        -e "s/^#define UC_FUZZ_MODE .*/#define UC_FUZZ_MODE ${mode}/" \
        fuzz_emu_x86_32.c > "${output}"
}

generate UC_ARCH_X86 UC_MODE_16 fuzz_emu_x86_16.c
generate UC_ARCH_X86 UC_MODE_64 fuzz_emu_x86_64.c
generate UC_ARCH_ARM UC_MODE_ARM fuzz_emu_arm_arm.c
generate UC_ARCH_ARM 'UC_MODE_ARM | UC_MODE_BIG_ENDIAN' fuzz_emu_arm_armbe.c
generate UC_ARCH_ARM UC_MODE_THUMB fuzz_emu_arm_thumb.c
generate UC_ARCH_ARM64 UC_MODE_ARM fuzz_emu_arm64_arm.c
generate UC_ARCH_ARM64 'UC_MODE_ARM | UC_MODE_BIG_ENDIAN' fuzz_emu_arm64_armbe.c
generate UC_ARCH_M68K UC_MODE_BIG_ENDIAN fuzz_emu_m68k_be.c
generate UC_ARCH_MIPS 'UC_MODE_MIPS32 | UC_MODE_BIG_ENDIAN' fuzz_emu_mips_32be.c
generate UC_ARCH_MIPS 'UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN' fuzz_emu_mips_32le.c
generate UC_ARCH_MIPS 'UC_MODE_MIPS64 | UC_MODE_BIG_ENDIAN' fuzz_emu_mips_64be.c
generate UC_ARCH_MIPS 'UC_MODE_MIPS64 | UC_MODE_LITTLE_ENDIAN' fuzz_emu_mips_64le.c
generate UC_ARCH_PPC 'UC_MODE_32 | UC_MODE_BIG_ENDIAN' fuzz_emu_ppc_32be.c
generate UC_ARCH_PPC 'UC_MODE_64 | UC_MODE_BIG_ENDIAN' fuzz_emu_ppc_64be.c
generate UC_ARCH_RISCV UC_MODE_RISCV32 fuzz_emu_riscv_32le.c
generate UC_ARCH_RISCV UC_MODE_RISCV64 fuzz_emu_riscv_64le.c
generate UC_ARCH_SPARC 'UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN' fuzz_emu_sparc_32be.c
generate UC_ARCH_SPARC 'UC_MODE_SPARC64 | UC_MODE_BIG_ENDIAN' fuzz_emu_sparc_64be.c
generate UC_ARCH_TRICORE UC_MODE_LITTLE_ENDIAN fuzz_emu_tricore_le.c
generate UC_ARCH_S390X UC_MODE_BIG_ENDIAN fuzz_emu_s390x_be.c
