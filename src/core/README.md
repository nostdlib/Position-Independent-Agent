[< Back to Source](../README.md) | [< Back to Project Root](../../README.md)

# Core Libraries

Zero-dependency foundations: hashing, encoding, strings, memory, math, type system, and formatting — all implemented without any CRT or standard library.

## DJB2 Hashing: Build-Unique Seeds

**File:** `algorithms/djb2.h`

The DJB2 hash is the foundation of all runtime symbol resolution (PEB module lookup, PE export matching). Two critical design choices make it non-trivial:

### Compile-Time Seeding via FNV-1a

The hash seed is **not** a fixed constant — it's generated at compile time from `__DATE__` using FNV-1a (offset basis `0x811C9DC5`, prime `0x01000193`). Every build produces different hash values, preventing static signature detection:

```c
constexpr UINT64 seed = FNV1a(__DATE__);  // different per compilation
constexpr UINT64 Hash(const TChar* str) {
    UINT64 hash = seed;
    while (*str) hash = (hash << 5) + hash + ToLowerCase(*str++);
    return hash;
}
```

### Dual Evaluation Modes

- `Hash()` — `constexpr`: can run at compile-time **or** runtime (for PEB module name matching)
- `HashCompileTime()` — `consteval`: **guaranteed** compile-time only (for embedding hash values in `.text` without string literals)

`HashCompileTime` uses deduced array size (`const TChar (&str)[N]`) which prevents accidental runtime calls with pointer arguments.

### Case-Insensitive Matching

Characters are lowercased before hashing — essential for Windows PE export name resolution where `"KERNEL32.DLL"` and `"kernel32.dll"` must produce the same hash.

## Base64: Lookup-Table-Free Encoding

**File:** `algorithms/base64.h`, `algorithms/base64.cc`

Standard Base64 implementations use a 64-byte lookup table (`A-Za-z0-9+/`) that would land in `.rdata`. Instead, the encoder computes output characters via arithmetic offset selection — no table, no data section dependency:

```c
// Arithmetic character mapping — replaces the standard 64-byte lookup table:
if (value < 26)      return value + 'A';       // A-Z
else if (value < 52) return value - 26 + 'a';  // a-z
else if (value < 62) return value - 52 + '0';  // 0-9
else if (value == 62) return '+';
else                  return '/';
```

The decoder returns `0xFF` for invalid characters, enabling inline error detection without separate validation passes. RFC 4648 compliant with proper `=` padding handling.

## String Operations: UTF-8 ↔ UTF-16 Without ICU

**File:** `string/string.h`, `string/string.cc`

Full bidirectional UTF-8/UTF-16 conversion with surrogate pair handling:

### UTF-8 → UTF-16

Decodes variable-length sequences (1-4 bytes) and produces surrogate pairs for codepoints ≥ U+10000:

```c
// For codepoint >= 0x10000 on systems with 2-byte WCHAR:
WCHAR high = 0xD800 + (codepoint >> 10);      // high surrogate
WCHAR low  = 0xDC00 + (codepoint & 0x3FF);    // low surrogate
```

### Float-to-String Without sprintf

Custom double-to-ASCII formatter: extracts integer part via truncation, fractional part via repeated multiplication by 10, handles rounding at the specified precision, and trims trailing zeros. No `snprintf`, no `dtoa`, no `__gcvt`.

## Memory Operations: Word-at-a-Time Optimization

**File:** `memory/memory.h`, `memory/memory.cc`

CRT-free `memset`, `memcpy`, `memmove`, `memcmp` with alignment-aware bulk operations:

### memset Optimization

```c
// 1. Byte-fill until pointer is USIZE-aligned
while ((USIZE)ptr & (sizeof(USIZE) - 1)) *ptr++ = value;

// 2. Replicate byte into full word: 0xAB → 0xABABABABABABABAB
USIZE word = value | (value << 8) | (value << 16) | (value << 24) | ...;

// 3. Store entire words (8 bytes on 64-bit)
while (remaining >= sizeof(USIZE)) { *(USIZE*)ptr = word; ptr += sizeof(USIZE); }

// 4. Byte-fill remainder
```

### memmove Overlap Detection

Forward copy when `dst < src`, backward copy when `dst > src` — prevents the classic overlapping-buffer corruption that `memcpy` doesn't handle.

### Apple-Specific: `__bzero`

macOS LLVM LTO backend may generate calls to `__bzero` (Apple's internal symbol) instead of `bzero`. The runtime implements both to prevent linker failures.

## Compiler Abstractions

**File:** `compiler/compiler.h`

### Entry Point Stack Alignment

On x86_64 POSIX, the kernel provides a 16-byte aligned stack — but there's **no `CALL` instruction** pushing a return address. The ABI requires 16-byte alignment **after** `CALL` (which pushes 8 bytes). The `force_align_arg_pointer` attribute re-aligns RSP:

```c
__attribute__((force_align_arg_pointer))
void entry_point() { ... }
```

Without this, SSE instructions that require 16-byte aligned operands will segfault.

### Preventing .rdata Generation

`__attribute__((optnone))` on functions that construct string/array constants prevents the optimizer from "helpfully" moving them to `.rodata`:

```c
OPTNONE void BuildString() {
    char s[] = "hello";  // stays on stack, not moved to .rodata
}
```

## PRNG: xorshift64

**File:** `math/prng.h`

Fast pseudo-random number generator with 2^64-1 period:

```c
state ^= state << 13;
state ^= state >> 7;
state ^= state << 17;
return state & 0x7FFFFFFF;  // lower 31 bits
```

Character generation uses multiplication-based range mapping instead of modulo (avoids division):

```c
// Map [0, 32767] → [0, 25] without division:
INT32 letter = ((value & 0x7FFF) * 26) >> 15;
return 'a' + letter;
```

## Result Type: Zero-Cost Tagged Union

**File:** `types/result.h`

```c
template<typename T, typename E>
class Result {
    union { T value; E error; };
    BOOL isOk;
};
```

- `void` specialization uses empty `VOID_TAG` struct (zero overhead)
- `[[nodiscard]]` forces callers to check success/failure
- `__is_trivially_destructible` intrinsic skips destructor calls when both T and E are trivial types
- Stack-only enforcement: `operator new` is deleted

## Bit Operations: Single-Instruction Rotation

**File:** `math/bitops.h`

The classic rotate pattern that compilers recognize and emit as a single `ROL`/`ROR` instruction:

```c
template<typename T>
constexpr T Rotr(T x, unsigned n) {
    return (x >> n) | (x << (sizeof(T) * 8 - n));
}
```

Used extensively in SHA-256/384 (`Sigma0`, `Sigma1`, `sigma0`, `sigma1` functions).

## String Formatting: Type-Safe Printf

**File:** `string/string_formatter.h`

Printf-style formatting without varargs or `va_list` — uses a type-erased `Argument` struct with enum discrimination:

```c
struct Argument {
    enum Type { Int, UInt, String, WString, Float, Error, ... };
    Type type;
    union { INT64 intVal; UINT64 uintVal; const CHAR* strVal; double floatVal; };
};
```

Output is written character-by-character through a callback (`bool (*writer)(void* ctx, TChar ch)`), enabling flexible destinations (buffer, console, network) without intermediate allocation. Supports field width, padding, precision, and left/right alignment.
