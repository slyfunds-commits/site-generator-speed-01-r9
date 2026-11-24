// cpuid_avx2.h — runtime AVX2 detection without libgcc.
// __builtin_cpu_supports("avx2") pulls in libgcc's __cpu_model, which a
// -nostdlib freestanding .node cannot link. This does the same check by
// hand (CPUID leaf 1 AVX+OSXSAVE, XCR0 YMM state, leaf 7 AVX2), in plain
// inline asm, safe under -fPIC (cpuid's EBX output is an asm output, so
// gcc saves/restores the PIC base register around it).
// Returns 1 only if AVX2 instructions are safe to execute on this CPU
// with this OS (YMM state saved across context switches).
#pragma once
#include <stdint.h>
static inline int cpu_has_avx2(void) {
  uint32_t eax, ebx, ecx, edx;
  // leaf 1: ECX bit 27 = OSXSAVE, bit 28 = AVX
  __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
  if (!(ecx & (1u << 27)) || !(ecx & (1u << 28))) return 0;
  // XCR0 (XCR 0): bits 1 (SSE) and 2 (AVX/YMM) must both be set
  uint32_t xcr0_lo, xcr0_hi;
  __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
  if ((xcr0_lo & 0x6u) != 0x6u) return 0;
  // leaf 7 subleaf 0: EBX bit 5 = AVX2
  __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
  return (ebx & (1u << 5)) != 0;
}
