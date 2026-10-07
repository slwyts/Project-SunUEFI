// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exact IEEE binary32/64 -> binary128 extension for the LP64 ARM64 app build.
typedef union { double f; unsigned long long u; } DOUBLE_BITS;
typedef union { long double f; unsigned long long u[2]; } QUAD_BITS;
_Static_assert(sizeof(long double) == 16, "ARM64 binary128 long double expected");
long double __extenddftf2(double value) {
  DOUBLE_BITS input = {.f=value}; QUAD_BITS output = {.u={0,0}};
  unsigned long long fraction = input.u & 0x000fffffffffffffULL;
  unsigned exponent = (unsigned)((input.u >> 52) & 0x7ff);
  unsigned long long sign = input.u & 0x8000000000000000ULL;
  if (exponent == 0x7ff) {
    output.u[1]=sign | 0x7fff000000000000ULL | (fraction >> 4);
    output.u[0]=fraction << 60;
    if (fraction != 0) { output.u[1] |= 0x0000800000000000ULL; }
  } else if (exponent != 0) {
    output.u[1]=sign | ((unsigned long long)(exponent+15360) << 48) | (fraction >> 4);
    output.u[0]=fraction << 60;
  } else if (fraction == 0) {
    output.u[1]=sign;
  } else {
    unsigned shift = (unsigned)__builtin_clzll(fraction)-11;
    fraction = (fraction << shift) & 0x000fffffffffffffULL;
    output.u[1]=sign | ((unsigned long long)(15361-shift) << 48) | (fraction >> 4);
    output.u[0]=fraction << 60;
  }
  return output.f;
}
long double __extendsftf2(float value) { return __extenddftf2((double)value); }
int __fpclassifyl(long double value) {
  QUAD_BITS bits={.f=value}; unsigned exponent=(unsigned)((bits.u[1] >> 48)&0x7fff);
  int fraction = bits.u[0] != 0 || (bits.u[1]&0x0000ffffffffffffULL) != 0;
  if (exponent == 0) { return fraction ? 3 : 2; }
  if (exponent == 0x7fff) { return fraction ? 0 : 1; }
  return 4;
}
