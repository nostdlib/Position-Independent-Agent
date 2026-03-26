# Memory and Strings Without a Standard Library

When you compile with `-nostdlib`, you lose every function the C runtime normally
provides. That includes `memset`, `memcpy`, `strlen`, `printf`, and everything else
you take for granted. This document walks through how this project rebuilds those
primitives from scratch, why each design decision was made, and where the subtle
pitfalls hide.

Source files covered:
- `src/core/memory/memory.cc` and `src/core/memory/memory.h`
- `src/core/string/string.cc` and `src/core/string/string.h`
- `src/core/string/string_formatter.h`
- `src/core/compiler/compiler.h` (for `COMPILER_RUNTIME`)

See also: [01-what-is-pic.md](01-what-is-pic.md) for why position-independent code
cannot reference global data, and [03-build-system.md](03-build-system.md) for the
`-nostdlib` and `-ffreestanding` flags that make all of this necessary.

---

## 1. Why Reimplement memset/memcpy/memcmp?

The obvious answer is "we don't have libc." But that undersells the problem.

Even when you pass `-nostdlib -ffreestanding` to the compiler, **the compiler itself
still generates implicit calls to `memset`, `memcpy`, and `memcmp`**. This happens
in situations you might not expect:

- Zeroing a struct: `MyStruct s = {};` compiles to a `memset` call.
- Copying a struct by value: `a = b;` compiles to a `memcpy` call.
- Comparing structs in some contexts emits `memcmp`.

These are not calls you wrote. The compiler inserted them. And if the linker cannot
find symbols named `memset`, `memcpy`, and `memcmp`, you get:

```
undefined reference to 'memset'
undefined reference to 'memcpy'
```

So the project provides its own implementations. They are declared `extern "C"` so
the linker resolves them by their standard C names, exactly as the compiler expects:

```cpp
extern "C" COMPILER_RUNTIME PVOID memset(PVOID dest, INT32 ch, USIZE count);
extern "C" COMPILER_RUNTIME PVOID memcpy(PVOID dest, PCVOID src, USIZE count);
extern "C" COMPILER_RUNTIME INT32 memcmp(PCVOID ptr1, PCVOID ptr2, USIZE num);
```

The `COMPILER_RUNTIME` attribute on each function is critical. More on that in
Section 5.

---

## 2. Word-at-a-Time Optimization in memset

A naive `memset` writes one byte per loop iteration. On a 64-bit machine with a
64-bit memory bus, that wastes 7/8 of each bus transaction. The implementation in
`memory.cc` uses a three-phase approach: align, blast words, clean up.

### The Algorithm

```cpp
extern "C" COMPILER_RUNTIME PVOID memset(PVOID dest, INT32 ch, USIZE count)
{
    PUCHAR d = (PUCHAR)dest;
    UCHAR byte = (UCHAR)ch;

    // Phase 1: Handle leading unaligned bytes
    while (count > 0 && ((USIZE)d & (sizeof(USIZE) - 1)) != 0)
    {
        *d++ = byte;
        count--;
    }

    // Phase 2: Word-at-a-time for aligned middle portion
    if (count >= sizeof(USIZE))
    {
        USIZE word = 0;
        for (USIZE b = 0; b < sizeof(USIZE); b++)
        {
            word |= (USIZE)byte << (b * 8);
        }

        PUSIZE w = (PUSIZE)d;
        while (count >= sizeof(USIZE))
        {
            *w++ = word;
            count -= sizeof(USIZE);
        }
        d = (PUCHAR)w;
    }

    // Phase 3: Handle trailing bytes
    while (count > 0)
    {
        *d++ = byte;
        count--;
    }

    return dest;
}
```

### Detailed Walkthrough

Consider `memset(buffer, 0xAB, 22)` where `buffer` starts at address `0x1002`
on a 64-bit system (word size = 8 bytes).

**Phase 1 -- Align to word boundary.**
The alignment mask is `sizeof(USIZE) - 1 = 7`, or binary `0b111`. The pointer
`0x1002` has its low 3 bits set to `010`, so it is not 8-byte aligned. The next
aligned address is `0x1008`. We write 6 bytes one at a time:

```
Address:  1002 1003 1004 1005 1006 1007 | 1008 ...
          [AB] [AB] [AB] [AB] [AB] [AB] |
          <--- 6 unaligned bytes ------> | <-- now aligned
count remaining: 22 - 6 = 16
```

**Phase 2 -- Replicate the byte into a full word, then write words.**
The byte `0xAB` gets replicated into all 8 positions of a 64-bit word:

```
Byte:   0xAB
Word:   0xABABABABABABABAB

Built by shifting:
  byte << 0  = 0x00000000000000AB
  byte << 8  = 0x000000000000AB00
  byte << 16 = 0x0000000000AB0000
  byte << 24 = 0x00000000AB000000
  byte << 32 = 0x000000AB00000000
  byte << 40 = 0x0000AB0000000000
  byte << 48 = 0x00AB000000000000
  byte << 56 = 0xAB00000000000000
  OR them all = 0xABABABABABABABAB
```

Now write 8 bytes at a time through a `USIZE*` pointer:

```
Address:  1008             1010             1018
          [ABABABABABABABAB][ABABABABABABABAB]
          <--- 8 bytes ---><--- 8 bytes --->
count remaining: 16 - 16 = 0
```

**Phase 3 -- Trailing bytes.**
In this example count is already 0, so nothing to do. If we had started with 23
bytes instead of 22, there would be 1 trailing byte written here.

The full picture:

```
            Phase 1           Phase 2                    Phase 3
          (byte-by-byte)    (word-at-a-time)           (byte-by-byte)
          +-+-+-+-+-+-+-----+----------------+--------+-+
 Address: |A|A|A|A|A|A|     |AAAAAAAA        |AAAAAAAA| |A|
          +-+-+-+-+-+-+     +----------------+--------+-+
          0x1002     0x1007 0x1008           0x100F    0x1017
          <-unaligned->     <-------- aligned words -------->
```

Why the alignment step matters: unaligned word writes are slow on x86 (they may
cross cache lines) and actually fault on some ARM configurations. The few extra
byte writes at the start pay for themselves many times over on any buffer larger
than a couple of words.

---

## 3. memcpy vs memmove: The Overlap Problem

`memcpy` and `memmove` both copy `count` bytes from `src` to `dest`. The
difference: `memcpy` has **undefined behavior** when source and destination
overlap. `memmove` handles it correctly.

### Why Overlap Breaks Forward Copying

```
Memory layout (addresses increase left to right):

src:   [ A  B  C  D  E ]
        ^
dest:      [ .  .  .  .  . ]
            ^
       src starts at 0x100, dest starts at 0x102, count = 5

Forward copy, step by step:
  copy src[0] -> dest[0]   (A -> 0x102)   But 0x102 IS src[2]!
  copy src[1] -> dest[1]   (B -> 0x103)   But 0x103 IS src[3]!
  copy src[2] -> dest[2]   ... src[2] is now 'A', not 'C'. Corrupted.
  copy src[3] -> dest[3]   ... src[3] is now 'B', not 'D'. Corrupted.
```

The source data gets overwritten before it is read.

### How memmove Solves It

The implementation checks whether `dest` is before or after `src`, then picks
the safe direction:

```cpp
extern "C" COMPILER_RUNTIME PVOID memmove(PVOID dest, PCVOID src, USIZE count)
{
    PUCHAR d = (PUCHAR)dest;
    const UCHAR *s = (const UCHAR *)src;

    if (d < s)
    {
        // Forward copy: destination is before source -- safe
        for (USIZE i = 0; i < count; i++)
            d[i] = s[i];
    }
    else if (d > s)
    {
        // Backward copy: destination overlaps after source -- copy from end
        for (USIZE i = count; i > 0; i--)
            d[i - 1] = s[i - 1];
    }

    return dest;
}
```

When `d > s` (the overlap case above), backward copying reads `src[4]` before
anything writes to it, then `src[3]`, and so on. Each byte is read before its
source location gets overwritten.

Note that the project's `memcpy` includes a word-at-a-time fast path for aligned
pointers, while `memmove` does not -- it stays byte-by-byte. This is a deliberate
tradeoff: `memmove` is called less often, and adding word-at-a-time logic for
both directions would double the code size for minimal gain in a position-
independent context where code size matters.

Also note that `memcpy` checks alignment of **both** pointers simultaneously:

```cpp
if (((USIZE)d | (USIZE)s) % sizeof(USIZE) == 0)
```

The bitwise OR trick: if either pointer has any low bits set, the OR result will
too, and the modulo check fails. Both pointers must be word-aligned for the fast
path to engage.

---

## 4. bzero and __bzero: Defensive LTO Stubs

```cpp
extern "C" COMPILER_RUNTIME VOID bzero(PVOID dest, USIZE count)
{
    memset(dest, 0, count);
}

extern "C" COMPILER_RUNTIME VOID __bzero(PVOID dest, USIZE count)
{
    memset(dest, 0, count);
}
```

Both are thin wrappers around `memset(..., 0, ...)`. `bzero` is a legacy POSIX
function. `__bzero` is an Apple/LLVM-specific variant.

Why they exist: LLVM's Link-Time Optimizer (LTO) sometimes decides to emit calls
to `bzero` or `__bzero` instead of `memset` when it sees a zero-fill pattern.
This happens **after** the source code has been compiled -- the optimizer rewrites
the already-compiled IR. If these symbols do not exist, you get a linker error
that is extremely confusing because your source code never mentions them.

These are purely defensive stubs. They cost a few bytes of code and save hours of
debugging when LTO decides to get creative.

---

## 5. The COMPILER_RUNTIME Attribute

Defined in `src/core/compiler/compiler.h`:

```cpp
#define COMPILER_RUNTIME __attribute__((noinline, used, optnone))
```

Every memory function carries this attribute. Each component is load-bearing:

**`noinline`** -- Prevents the compiler from inlining the function at call sites.
If `memcpy` gets inlined, the optimizer sees the loop body and may "improve" it
by emitting SIMD instructions. SIMD instructions reference constant vectors stored
in `.rodata` -- a data section. Position-independent code cannot have data sections
(see [01-what-is-pic.md](01-what-is-pic.md)). Keeping the function out-of-line
means the optimizer never gets the chance.

**`used`** -- Tells the compiler "do not remove this function as dead code." The
compiler generates implicit calls to `memset`/`memcpy` that are invisible to the
optimizer's reachability analysis. Without `used`, the optimizer might conclude
that nobody calls `memset`, strip it, and then the linker fails on the implicit
calls the optimizer itself inserted.

**`optnone`** -- Disables all optimization passes on this specific function. Even
with `noinline`, the compiler can still optimize the function body itself. Without
`optnone`, the optimizer might:
- Replace the byte-replication loop with a constant loaded from `.rodata`.
- Introduce SIMD instructions that reference vector constants.
- Unroll loops in ways that increase code size unpredictably.

All three attributes must be present. Remove `noinline` and the function gets
inlined and optimized at each call site. Remove `used` and it gets stripped.
Remove `optnone` and its body gets rewritten with data-section references. Any
one of these failures breaks position independence.

This attribute is also used on compiler runtime functions for 32-bit arithmetic
helpers (see `src/core/compiler/compiler_runtime.riscv32.cc`), where the same
reasoning applies: the compiler generates implicit calls to `__udivdi3`,
`__umoddi3`, etc., and those functions must not be inlined, removed, or optimized
into forms that reference data sections.

---

## 6. String Functions Without libc

The `StringUtils` class in `src/core/string/string.h` reimplements standard string
operations. All functions are templates constrained by the `TCHAR` concept:

```cpp
template <typename TChar>
concept TCHAR = __is_same_as(TChar, CHAR) || __is_same_as(TChar, WCHAR);
```

This means every string function works with both narrow (`CHAR`) and wide (`WCHAR`)
strings from a single template definition. No code duplication.

### Length

Walks forward until it hits a null terminator. Nothing clever, nothing that needs
to be clever:

```cpp
template <TCHAR TChar>
constexpr USIZE StringUtils::Length(const TChar *p) noexcept
{
    if (!p)
        return 0;
    USIZE i = 0;
    while (p[i] != (TChar)'\0')
        i++;
    return i;
}
```

### Compare (with case-insensitive support)

Character-by-character comparison with an optional `ignoreCase` flag. The
case-insensitive path matters on Windows, where DLL names like `KERNEL32.DLL`
and `kernel32.dll` must compare equal:

```cpp
template <TCHAR TChar>
constexpr BOOL StringUtils::Compare(const TChar *s1, const TChar *s2,
                                    BOOL ignoreCase) noexcept
{
    if (!s1 || !s2)
        return s1 == s2;
    const TChar *str1 = s1;
    const TChar *str2 = s2;
    while (*str1 && *str2)
    {
        TChar c1 = ignoreCase ? ToLowerCase(*str1) : *str1;
        TChar c2 = ignoreCase ? ToLowerCase(*str2) : *str2;
        if (c1 != c2)
            return false;
        str1++;
        str2++;
    }
    return *str1 == *str2;
}
```

### Character Classification and Conversion

`IsSpace`, `IsDigit`, `IsAlpha`, `ToLowerCase`, `ToUpperCase` -- all hand-rolled
with explicit range checks. No lookup tables (those would live in `.rodata`).
Everything is `constexpr FORCE_INLINE`, meaning these compile down to a handful
of comparison instructions with zero function call overhead.

### No Dynamic Allocation

None of these functions allocate memory. All operations work on caller-provided
fixed-size buffers wrapped in `Span<T>`, which carries a pointer and a size.
The caller is responsible for providing a buffer large enough. This is a deliberate
constraint: position-independent code running in shellcode contexts has no heap.

---

## 7. Float-to-String Conversion: Step by Step

`FloatToStr` in `src/core/string/string.cc` is a manual implementation of what
`printf("%f", value)` does internally. Without the C runtime, we have to extract
digits ourselves.

### The Algorithm

Given `FloatToStr(3.14159, buffer, 4)` (4 decimal places):

**Step 1: Handle sign.**
Value is positive, so skip. If negative, write `'-'` and negate.

**Step 2: Apply rounding correction.**
For `precision = 4`, compute `0.5 / 10^4 = 0.5 / 10000 = 0.00005`. Add it:
`3.14159 + 0.00005 = 3.14164`. This ensures that the truncation in later steps
produces correctly rounded output.

Why this works: we are about to truncate after 4 fractional digits. Without
rounding, `3.14159` truncated to 4 places gives `3.1415`. With the correction,
`3.14164` truncated to 4 places gives `3.1416` -- the correctly rounded result.

```cpp
double scale = 1.0;
for (UINT8 p = 0; p < precision; p++)
    scale = scale * 10.0;
value = value + 5.0 / (scale * 10.0);
```

**Step 3: Split integer and fractional parts.**

```cpp
UINT64 intPart = (UINT64)value;       // 3
double fracPart = value - (double)intPart;  // 0.14164
```

**Step 4: Convert integer part to string.**
Delegates to `UIntToStr`, which does standard digit extraction (divide by 10,
take remainder, reverse). Result: `"3"`.

**Step 5: Write decimal point.**
Append `'.'`.

**Step 6: Extract fractional digits one at a time.**
Multiply by 10, truncate to get the digit, subtract it off, repeat:

```
fracPart = 0.14164
  * 10 = 1.4164  -> digit '1', fracPart = 0.4164
  * 10 = 4.164   -> digit '4', fracPart = 0.164
  * 10 = 1.64    -> digit '1', fracPart = 0.64
  * 10 = 6.4     -> digit '6', fracPart = 0.4
```

Result so far: `"3.1416"`.

Note the clamping in the actual code:

```cpp
INT32 digit = (INT32)fracPart;
if (digit < 0) digit = 0;
if (digit > 9) digit = 9;
```

Floating-point arithmetic is imprecise. Without these guards, accumulated error
could produce a digit value of 10 or -1, which would write a non-digit character
into the buffer.

**Step 7: Trim trailing zeros.**

```cpp
while (pos > 2 && buffer[pos - 1] == '0' && buffer[pos - 2] != '.')
    pos--;
```

If the result were `"3.1400"`, this trims it to `"3.14"`. The check
`buffer[pos - 2] != '.'` ensures we never trim `"3.0"` down to `"3."`.

---

## 8. StringFormatter: Type Erasure and the Callback Pattern

`StringFormatter` in `src/core/string/string_formatter.h` provides `printf`-style
formatting without C variadic arguments (`va_list`). The design solves two problems
simultaneously: type safety and output flexibility.

### The Problem with va_list

C's `printf` uses `va_list` to access variadic arguments. This is fundamentally
type-unsafe: if the format string says `%d` but the argument is a `double`, you
get silent data corruption or a crash. The compiler cannot check this at compile
time in a freestanding environment.

### Type Erasure via the Argument Struct

The solution uses C++ variadic templates to capture types at compile time, then
erases them into a uniform representation. Each argument becomes an `Argument`
struct:

```cpp
struct Argument
{
    enum class Type
    {
        INT32, UINT32, INT64, UINT64,
        DOUBLE, CSTR, WSTR, PTR, ERROR_VALUE
    };

    Type Kind;
    union
    {
        INT32 I32;
        UINT32 U32;
        INT64 I64;
        UINT64 U64;
        double Dbl;
        const CHAR *Cstr;
        const WCHAR *Wstr;
        PVOID Ptr;
        Error ErrValue;
    };

    // Overloaded constructors select the right union member
    Argument(INT32 v) : Kind(Type::INT32), I32(v) {}
    Argument(double v) : Kind(Type::DOUBLE), Dbl(v) {}
    Argument(const CHAR *v) : Kind(Type::CSTR), Cstr(v) {}
    // ... one constructor per supported type
};
```

The key insight: the constructor overloads act as the type-erasure mechanism.
When you write `Argument(42)`, the `INT32` constructor fires, setting
`Kind = Type::INT32` and storing the value in `I32`. When you write
`Argument(3.14)`, the `double` constructor fires instead. The concrete type is
captured once in the `Kind` discriminator, and from that point on, the formatter
dispatches via `Kind` without needing to know the original type.

### How Format() Packs Arguments

```cpp
template <TCHAR TChar, typename... Args>
static INT32 Format(BOOL (*writer)(PVOID, TChar), PVOID context,
                    const TChar *format, Args &&...args)
{
    if constexpr (sizeof...(Args) == 0)
    {
        return FormatWithArgs<TChar>(writer, context, format,
                                     Span<const Argument>());
    }
    else
    {
        Argument argArray[] = {Argument(args)...};
        return FormatWithArgs<TChar>(writer, context, format,
                                     Span<const Argument>(argArray));
    }
}
```

The variadic template `Args...` captures the caller's argument types at compile
time. The pack expansion `{Argument(args)...}` constructs one `Argument` per
value, and the array is wrapped in a `Span` for bounds-safe access. After this
point, `FormatWithArgs` works entirely with `Span<const Argument>` -- it has no
knowledge of the original types.

This also means `FormatWithArgs` is instantiated only once per character type
(CHAR or WCHAR), not once per unique combination of argument types. That keeps
code size small -- important for position-independent code.

### The Writer Callback Pattern

The formatter never allocates a buffer. Instead, it writes one character at a
time through a callback:

```cpp
BOOL (*writer)(PVOID context, TChar ch)
```

The callback returns `true` to continue or `false` to stop (buffer full, write
error, etc.). The `PVOID context` parameter lets the caller attach arbitrary
state -- a buffer position, a file handle, a network socket, anything:

```cpp
// Example: writing into a fixed buffer
struct BufferContext {
    CHAR* buffer;
    USIZE size;
    USIZE pos;
};

BOOL BufferWriter(PVOID ctx, CHAR ch) {
    BufferContext* c = (BufferContext*)ctx;
    if (c->pos >= c->size - 1) return false;  // buffer full
    c->buffer[c->pos++] = ch;
    return true;
}

// Usage
BufferContext ctx = { buffer, sizeof(buffer), 0 };
StringFormatter::Format(BufferWriter, &ctx, "Value: %d", 42);
ctx.buffer[ctx.pos] = '\0';
```

This design has several properties that matter for position-independent code:

1. **Zero allocation.** The formatter itself never calls `malloc` or touches a
   heap. The caller owns the output destination entirely.

2. **Flexible output.** The same formatting engine can write to a stack buffer,
   a console via syscall, a UEFI console protocol, or a network socket. Only
   the callback changes.

3. **Bounded output.** The callback can enforce a size limit and stop formatting
   early by returning `false`. Every write path in `FormatWithArgs` checks the
   return value and bails out immediately on `false`.

4. **No data sections.** The format string is passed by the caller (typically
   built on the stack in PIC contexts). The formatter itself contains no string
   literals or constant data.

### How FormatWithArgs Dispatches

The core loop in `FormatWithArgs` walks the format string character by character.
When it encounters `%`, it parses optional flags (`-`, `0`, `#`), field width,
precision, and length modifiers (`l`, `ll`, `z`), then dispatches to the
appropriate formatting function based on the specifier:

- `%d` / `%u` -> `FormatInt64` / `FormatUInt64`
- `%x` / `%X` -> `FormatUInt64AsHex`
- `%f` -> `FormatDouble`
- `%s` -> inline string copy through writer
- `%ws` / `%ls` -> `FormatWideString`
- `%p` -> `FormatPointerAsHex`
- `%e` -> `FormatError` (project-specific: formats the `Error` type's cause chain)
- `%%` -> literal `%`

Each internal formatter follows the same pattern: build digits into a small stack
buffer, apply padding/alignment, then emit through the writer callback one
character at a time. None of them allocate. None of them reference data sections.

---

## Summary

The memory and string layer sits at the very bottom of the project's dependency
graph. Everything else -- the logger, the PE parser, the syscall layer -- depends
on these primitives existing and being position-independent. The constraints are
rigid: no libc, no data sections, no heap, no SIMD, no inlining of runtime
functions. Within those constraints, the implementations are as straightforward
as possible. The word-at-a-time optimization in `memset` and `memcpy` is the one
concession to performance, and it pays for itself on any buffer over 16 bytes.

For the build flags that enforce all of this, see [03-build-system.md](03-build-system.md).
For how these primitives get used in the rest of the agent, start with the
[ARCHITECTURE-MAP.md](ARCHITECTURE-MAP.md) to see where `core/memory` and
`core/string` appear in the dependency graph.
