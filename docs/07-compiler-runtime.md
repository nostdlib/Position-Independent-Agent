# Compiler Runtime: What Happens Below the Language

When you write `uint64_t a = b / c` in C++, you assume the CPU handles it.
On a 64-bit machine, it does -- one instruction. On a 32-bit machine, the
hardware divider only works on 32-bit operands. The compiler emits a call to a
helper function -- `__udivdi3` on x86, `__aeabi_uidiv` on ARM -- and expects
something to provide it at link time.

Normally that something is libgcc or compiler-rt. We build with `-nostdlib`, so
those libraries are gone. If we do not supply every helper the compiler expects,
the linker fails with an undefined symbol error.

**Source files:**
- `src/core/compiler/compiler_runtime.i386.cc` -- x86 32-bit helpers
- `src/core/compiler/compiler_runtime.armv7a.cc` -- ARM EABI helpers
- `src/core/compiler/compiler_builtins.h` -- RISC-V CLZ/CTZ replacements

**Cross-references:** [03-build-system.md](03-build-system.md) explains the
`-nostdlib` flag. [04-entry-point.md](04-entry-point.md) covers how execution
reaches the point where these functions matter. [05-core-types.md](05-core-types.md)
documents the `UINT64`, `INT32`, and other type aliases. [01-what-is-pic.md](01-what-is-pic.md)
explains why data section references break position independence (relevant to
the RISC-V section). [ARCHITECTURE-MAP.md](ARCHITECTURE-MAP.md) shows where the
compiler runtime sits in the layer stack.

---

## Why 32-bit Needs Division Helpers

A 64-bit CPU has a 64-bit divider in hardware. A 32-bit CPU does not. Some
32-bit chips lack even a 32-bit hardware divide instruction. The compiler emits
a function call and moves on.

| Architecture | Division helpers needed |
|---|---|
| i386 | `__udivdi3`, `__umoddi3`, `__divdi3`, `__moddi3` |
| ARMv7-A | `__aeabi_uidiv`, `__aeabi_uidivmod`, `__aeabi_idiv`, `__aeabi_idivmod`, `__aeabi_uldivmod`, `__aeabi_ldivmod` |
| RISC-V 32 | Same as i386, plus CLZ/CTZ workarounds |

ARM also needs 64-bit shift helpers (`__aeabi_llsr`, `__aeabi_llsl`) and
floating-point conversion helpers (`__aeabi_l2d`, `__aeabi_d2lz`). The compiler
generates calls to all of these automatically. You never see them in source, but
the linker will remind you they are missing.

---

## Binary Long Division

The core algorithm behind every division helper is binary long division -- same
long division you learned in school, carried out in base 2.

### Worked Example: 13 / 3

```
13 in binary: 1101
 3 in binary: 11

         0100   <-- quotient (4)
      -------
  11 | 1101
       11       subtract at bit 2: 1101 - 1100 = 0001
       ---
       0001
         11     can't subtract at bit 1 (01 < 11)
          11    can't subtract at bit 0 (01 < 11)

Quotient:  0100 = 4
Remainder: 0001 = 1
Check:     4 * 3 + 1 = 13
```

Start at the most significant bit. At each position, shift the running remainder
left by one, pull in the next numerator bit, and check: is the remainder >= the
divisor? If yes, subtract and set that quotient bit to 1. If no, move on.

Here is the i386 implementation (`compiler_runtime.i386.cc`, lines 72-91):

```cpp
const INT32 startBit = 63 - __builtin_clzll(numerator);
UINT64 q = 0;
UINT64 r = 0;

for (INT32 i = startBit; i >= 0; i--)
{
    r <<= 1;
    if ((numerator >> i) & 1ULL)
        r |= 1ULL;
    if (r >= denominator)
    {
        r -= denominator;
        q |= (1ULL << i);
    }
}
```

`__builtin_clzll` finds leading zeros in the numerator. Subtracting from 63
gives the MSB position. Starting the loop there skips leading zeros -- a
division of two 32-bit values stored in `uint64_t` would otherwise waste 32
iterations doing nothing.

The ARM 32-bit variant (`compiler_runtime.armv7a.cc`, lines 74-86) is identical
in structure, just operating on 32-bit values with `__builtin_clz` and `1U`.

Both are marked `FORCE_INLINE` (`__attribute__((always_inline)) inline`). The
public functions like `__udivdi3` are thin wrappers returning either the
quotient or remainder.

---

## Power-of-2 Fast Paths

Before entering the loop, every division helper checks whether the divisor is a
power of 2. If so, division becomes a shift and modulo becomes a bitmask.

```cpp
// compiler_runtime.i386.cc, line 54
if (__builtin_expect((denominator & (denominator - 1ULL)) == 0, 1))
{
    const UINT32 shift = __builtin_ctzll(denominator);
    *quotient = numerator >> shift;
    *remainder = numerator & (denominator - 1ULL);
    return;
}
```

Why does `(x & (x - 1)) == 0` detect powers of 2? A power of 2 has exactly one
bit set: 8 = `1000`. Subtracting 1 flips it: 7 = `0111`. AND gives `0000`. Any
non-power-of-2 has at least one surviving bit.

`__builtin_ctzll` tells us which power. Dividing by 2^n is `>> n`. The
remainder is `& (denominator - 1)`. The source estimates ~20% of runtime
divisions hit this O(1) path.

There is also an early exit when numerator < denominator (quotient = 0,
remainder = numerator). Between these fast paths, most trivial divisions never
enter the loop.

---

## Naked Functions on ARM

On x86, division helpers are normal C++ functions. On ARM, the 64-bit division
helpers cannot be, because the EABI calling convention demands specific register
layouts that the compiler's prologue/epilogue would break.

`__attribute__((naked))` strips all compiler-generated code from a function.
No prologue, no epilogue, no register saves. The entire body is inline assembly:

```cpp
// compiler_runtime.armv7a.cc, lines 355-370
COMPILER_RUNTIME
__attribute__((naked)) void __aeabi_uldivmod(void)
{
    __asm__ volatile(
        "push   {r4, r5, lr}\n\t"
        "sub    sp, sp, #16\n\t"     // [quotient:8][remainder:8]
        "mov    r4, sp\n\t"          // r4 = &quotient
        "add    r5, sp, #8\n\t"      // r5 = &remainder
        "mov    r12, #0\n\t"         // is_signed = false
        "push   {r4, r5, r12}\n\t"
        "bl     divmod64_helper\n\t"
        "add    sp, sp, #12\n\t"
        "pop    {r0-r3}\n\t"         // quot->r0:r1, rem->r2:r3
        "pop    {r4, r5, pc}\n\t"
    );
}
```

The EABI dictates that `__aeabi_uldivmod` receives numerator in `r0:r1`,
denominator in `r2:r3`, and returns quotient in `r0:r1`, remainder in `r2:r3`.
The naked function manually manages the stack, calls the C++ helper
`divmod64_helper`, and loads results into the exact required registers via
`pop {r0-r3}`. The signed variant `__aeabi_ldivmod` is identical except it sets
`r12 = 1` (is_signed = true).

`divmod64_helper` itself is a normal C++ function. The naked wrapper exists
solely to bridge EABI register conventions and standard C++ calling semantics.

---

## ARM EABI Calling Conventions

The ARM EABI (Embedded Application Binary Interface) differs from x86:

| | x86 32-bit | ARM EABI |
|---|---|---|
| First 4 args | Stack | r0, r1, r2, r3 |
| Return value | EAX (or EAX:EDX) | r0 (or r0:r1) |
| 64-bit arg | Two stack slots | Register pair (r0:r1 or r2:r3) |
| Division result | Quotient only | Quotient AND remainder |

The last row is critical. On x86, `__udivdi3` returns only the quotient; you
call `__umoddi3` separately for the remainder. On ARM, `__aeabi_uidivmod`
returns both. The 32-bit divmod uses a packing trick:

```cpp
// compiler_runtime.armv7a.cc, lines 193-200
COMPILER_RUNTIME UINT64 __aeabi_uidivmod(UINT32 numerator, UINT32 denominator)
{
    UINT32 remainder = 0;
    UINT32 quotient = udiv32_internal(numerator, denominator, &remainder);
    return ((UINT64)remainder << 32) | quotient;
}
```

Returning `UINT64` places low 32 bits in r0 (quotient) and high 32 bits in r1
(remainder) -- exactly where the EABI expects them.

---

## The __aeabi_memset Argument Order Gotcha

Standard C `memset`:
```c
memset(dest, value, size);
```

ARM EABI `__aeabi_memset`:
```c
__aeabi_memset(dest, size, value);  // size and value are SWAPPED
```

The implementation (`compiler_runtime.armv7a.cc`, lines 694-697):

```cpp
COMPILER_RUNTIME void __aeabi_memset(void *dest, USIZE n, INT32 c)
{
    memset(dest, c, n);
}
```

The wrapper swaps `n` and `c` back to standard order. Get this wrong in your
implementation and memory gets silently corrupted. No crash, no warning -- just
wrong data at wrong offsets. The ARM compiler emits calls to `__aeabi_memset`
instead of standard `memset` in many contexts.

The `__aeabi_memcpy` variants do not have this problem -- their argument order
matches standard `memcpy`. Only `memset` is swapped. The trap is documented in
the [ARM EABI spec](https://github.com/ARM-software/abi-aa/blob/main/rtabi32/rtabi32.rst).

---

## RISC-V CLZ/CTZ Replacements

The division algorithm depends on `__builtin_clzll` and `__builtin_ctzll`. On
x86 and ARM, these map to single hardware instructions. On RISC-V 32-bit
without the Zbb extension, there is no such instruction.

Clang's fallback uses a De Bruijn multiplication lookup table that lands in
`.rodata`. As covered in [01-what-is-pic.md](01-what-is-pic.md), any reference
to a fixed data section breaks position independence. The compiler's default
implementation is unusable.

The fix in `compiler_builtins.h` is a binary search using no table at all:

```cpp
// compiler_builtins.h, lines 44-53
static FORCE_INLINE __UINT32_TYPE__ __pir_clz32(__UINT32_TYPE__ x)
{
    __UINT32_TYPE__ c = 0;
    if ((x & 0xFFFF0000) == 0) { c += 16; x <<= 16; }
    if ((x & 0xFF000000) == 0) { c += 8;  x <<= 8;  }
    if ((x & 0xF0000000) == 0) { c += 4;  x <<= 4;  }
    if ((x & 0xC0000000) == 0) { c += 2;  x <<= 2;  }
    if ((x & 0x80000000) == 0) { c += 1;             }
    return c;
}
```

It halves the search space each step: are the upper 16 bits all zero? Add 16,
shift left. Upper 8 bits zero? Add 8, shift. Continue with 4, 2, 1. Five
comparisons, constant time, no data section.

The 64-bit version splits into two 32-bit halves:

```cpp
// compiler_builtins.h, lines 55-61
static FORCE_INLINE __UINT32_TYPE__ __pir_clzll(__UINT64_TYPE__ v)
{
    const __UINT32_TYPE__ hi = (__UINT32_TYPE__)(v >> 32);
    if (hi != 0)
        return __pir_clz32(hi);
    return 32 + __pir_clz32((__UINT32_TYPE__)v);
}
```

CTZ mirrors the pattern, checking least significant bits and shifting right:

```cpp
// compiler_builtins.h, lines 25-33
static FORCE_INLINE __UINT32_TYPE__ __pir_ctz32(__UINT32_TYPE__ x)
{
    __UINT32_TYPE__ c = 0;
    if ((x & 0x0000FFFF) == 0) { c += 16; x >>= 16; }
    if ((x & 0x000000FF) == 0) { c += 8;  x >>= 8;  }
    if ((x & 0x0000000F) == 0) { c += 4;  x >>= 4;  }
    if ((x & 0x00000003) == 0) { c += 2;  x >>= 2;  }
    if ((x & 0x00000001) == 0) { c += 1;             }
    return c;
}
```

Macros at the bottom of the header redirect the compiler builtins:

```cpp
#define __builtin_ctzll(x) __pir_ctzll(x)
#define __builtin_clzll(x) __pir_clzll(x)
```

The division `.cc` files need no `#ifdef` guards for RISC-V. They call
`__builtin_clzll` as usual; the preprocessor swaps in the safe version. The
header uses Clang's predefined types (`__UINT32_TYPE__`) rather than the
project's aliases because it may be included before `primitives.h`. The entire
header is guarded by `#if defined(ARCHITECTURE_RISCV32)` and has no effect on
x86 or ARM.

---

## Floating-Point Conversion

ARM 32-bit without a hardware FPU needs software conversions between `int64` and
`double`. The compiler emits calls to `__aeabi_l2d` and `__aeabi_d2lz`
whenever you cast between these types.

### Integer to Double: __aeabi_l2d

IEEE-754 double: 1 sign bit, 11 exponent bits, 52 mantissa bits. The conversion
constructs this bit pattern manually:

```cpp
// compiler_runtime.armv7a.cc, lines 470-508 (simplified)
COMPILER_RUNTIME UINT64 __aeabi_l2d(INT64 val)
{
    if (val == 0) return 0ULL;

    BOOL negative = val < 0;
    UINT64 absVal = negative ? (UINT64)(-val) : (UINT64)val;
    const INT32 msb = 63 - __builtin_clzll(absVal);
    const INT32 exponent = 1023 + msb;          // IEEE-754 bias

    UINT64 mantissa = absVal;
    if (msb >= 52) mantissa >>= (msb - 52);     // lose low bits
    else           mantissa <<= (52 - msb);
    mantissa &= 0x000FFFFFFFFFFFFFULL;           // clear implicit 1

    UINT64 sign = negative ? 0x8000000000000000ULL : 0ULL;
    return sign | ((UINT64)exponent << 52) | mantissa;
}
```

The MSB position becomes the exponent (biased by 1023). The leading 1 is
implicit in IEEE-754 ("hidden bit"), so it gets masked off. Integers with more
than 52 significant bits lose precision -- that is the limit of `double`.

The function returns `UINT64` rather than `double` because on soft-float ARM,
doubles travel in integer register pairs. `INT64_MIN` is handled as a special
case to avoid signed overflow during negation.

### Double to Integer: __aeabi_d2lz

The reverse disassembles the IEEE-754 bit pattern:

```cpp
// compiler_runtime.armv7a.cc, lines 556-593 (simplified)
COMPILER_RUNTIME INT64 __aeabi_d2lz(UINT64 bits)
{
    UINT64 signBit      = bits & 0x8000000000000000ULL;
    UINT64 mantissaBits = bits & 0x000FFFFFFFFFFFFFULL;
    INT32 exponent = (INT32)((bits & 0x7FF0000000000000ULL) >> 52) - 1023;

    if (exponent < 0)  return 0LL;
    if (exponent >= 63) return signBit ? 0x8000000000000000LL
                                       : 0x7FFFFFFFFFFFFFFFLL;

    UINT64 mantissaWithOne = mantissaBits | 0x0010000000000000ULL;
    UINT64 intValue = (exponent <= 52)
        ? mantissaWithOne >> (52 - exponent)
        : mantissaWithOne << (exponent - 52);

    return signBit ? -(INT64)intValue : (INT64)intValue;
}
```

The exponent tells you how far to shift the mantissa. Exponent 0 means the
value is between 1 and 2. Exponent 52 means mantissa bits map directly to the
integer. The "z" in `d2lz` means truncation toward zero. Overflow clamps to
INT64_MIN or INT64_MAX. The unsigned variant `__aeabi_d2ulz` returns 0 for
negative inputs and UINT64_MAX for overflow.

---

## Summary

Every function is attributed with `COMPILER_RUNTIME`, which expands to
`__attribute__((noinline, used, optnone))`. `used` prevents the linker from
stripping functions that only the compiler calls. `optnone` prevents the
optimizer from transforming division back into a call to the function it is
implementing (infinite recursion). `noinline` ensures a callable symbol exists.

The ARM runtime compiles to approximately 1.5KB at `-Os`. No heap allocations,
fully reentrant, thread-safe. Peak stack usage is 32 bytes in the 64-bit
division paths.

None of this is something you would normally think about. It is invisible --
until you remove the standard library and suddenly it is not.
