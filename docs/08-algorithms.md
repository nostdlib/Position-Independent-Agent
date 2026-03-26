# Algorithms and Math: Hashing, Encoding, and Randomness

Position-independent code cannot call `rand()`, cannot use `htons()`, and cannot
store a Base64 lookup table in `.rodata`. Yet the project needs hashing for API
resolution, encoding for WebSocket framing, pseudorandomness for file names, byte
swapping for network protocols, and bit rotation for cryptographic primitives. This
document covers how all of that gets rebuilt from first principles using nothing but
arithmetic, bitwise operations, and the compiler's own constant-evaluation machinery.

Source files covered:
- `src/core/algorithms/djb2.h`
- `src/core/algorithms/base64.cc` and `base64.h`
- `src/core/math/prng.h`
- `src/core/math/byteorder.h`
- `src/core/math/bitops.h`

Prerequisites: [01-what-is-pic.md](01-what-is-pic.md) explains why data sections
are forbidden. [05-core-types.md](05-core-types.md) covers the `UINT32`, `UINT64`,
`Span`, and other custom types referenced throughout. [06-memory-and-strings.md](06-memory-and-strings.md)
discusses the broader `-nostdlib` constraint that motivates every design choice here.

---

## 1. DJB2 Hashing for API Resolution

The project resolves Windows API functions without storing their names as strings.
No `"LoadLibraryA"` in the binary, no `.rodata` section to scan for signatures.
Instead, it uses DJB2 -- a hash function published by Daniel J. Bernstein in 1991 on
comp.lang.c -- to convert function names into integer constants at compile time, then
matches those constants against hashes computed at runtime by walking the PE export table.

The core recurrence is simple:

```
hash = hash * 33 + character
```

Implemented as a shift-and-add to avoid a multiply instruction:

```cpp
h = ((h << 5) + h) + (UINT64)c;
```

`(h << 5) + h` is exactly `h * 32 + h`, which is `h * 33`. The character is folded
in by addition. Each iteration mixes the current hash state with one character of
input.

The full runtime implementation from `djb2.h`:

```cpp
template <typename TChar>
static constexpr FORCE_INLINE UINT64 Hash(const TChar *value)
{
    UINT64 h = Seed;
    for (UINT64 i = 0; value[i] != (TChar)0; ++i)
    {
        TChar c = StringUtils::ToLowerCase(value[i]);
        h = ((h << 5) + h) + (UINT64)c;
    }
    return h;
}
```

Two details worth noting. First, every character is lowercased before hashing because
Windows DLL export names can vary in casing across OS versions. Second, the hash is
64-bit, not 32-bit -- this reduces collision probability when hashing a large export
table down to effectively zero for practical purposes.

The usage pattern in the codebase looks like this:

```cpp
// At compile time: produce an integer constant
constexpr UINT64 TARGET = Djb2::HashCompileTime("LoadLibraryA");

// At runtime: walk PE exports, hash each name, compare
if (Djb2::Hash(exportName) == TARGET)
{
    // Found it
}
```

The constant `TARGET` embeds directly in the instruction stream as an immediate
operand. No string, no data section, no signature to match against.

---

## 2. Compile-Time Seed from \_\_DATE\_\_

There is an obvious weakness in the scheme described above. If the DJB2 seed is fixed
(the traditional value is 5381), then the hash of `"LoadLibraryA"` is the same constant
in every binary that uses this technique. Antivirus products could simply maintain a
table of known hash values and flag any binary containing them.

The fix is to derive the seed from `__DATE__` -- the compiler-provided macro that
expands to a string like `"Mar 25 2026"`. A binary compiled today has a different
seed than one compiled tomorrow, so every hash constant changes between builds.

The seed itself is computed via FNV-1a, a different hash function with well-known
parameters from the IETF draft specification:

```cpp
consteval UINT64 ct_hash_str_seed(const CHAR *s)
{
    UINT64 h = (UINT64)2166136261u;  // FNV-1 offset basis
    for (UINT64 i = 0; s[i] != '\0'; ++i)
        h = (h ^ (UINT64)(UINT8)s[i]) * (UINT64)16777619u;  // FNV-1 prime
    return h;
}
```

The seed is stored as a `static constexpr` member of the `Djb2` class:

```cpp
static constexpr UINT64 Seed = ct_hash_str_seed(__DATE__);
```

Note the `consteval` on the seed function. This is not `constexpr`. It is a hard
guarantee that the function runs during compilation and never at runtime. The
`__DATE__` string never touches the binary, and the FNV-1a logic never emits a
single runtime instruction. All that remains is the resulting integer.

---

## 3. constexpr vs consteval -- Why Both Exist

The `Djb2` class provides two hashing entry points, and the distinction between them
is not cosmetic.

`Hash()` is marked `constexpr`. That means it *can* run at compile time if you hand
it a constant expression, but it is also perfectly legal to call it with a `char*`
pointer discovered at runtime. The compiler may or may not fold a compile-time call --
there is no enforcement. This is the function used at runtime to hash export names
found while walking ntdll.dll's export table.

`HashCompileTime()` is marked `consteval`. It *must* run at compile time. If the
compiler cannot evaluate it during compilation, the program does not compile. Its
signature enforces this:

```cpp
template <typename TChar, USIZE N>
static consteval UINT64 HashCompileTime(const TChar (&value)[N])
```

The parameter is `const TChar (&value)[N]` -- a reference to a fixed-size array. This
only accepts string literals or `constexpr` arrays. You cannot pass a `char*` pointer
to it. If you tried, the compiler would reject it with a type error before the
`consteval` enforcement even came into play.

Both functions implement the same algorithm. The split exists to serve two distinct
roles: `HashCompileTime` produces the compile-time constants that become immediate
operands in comparison instructions, and `Hash` produces the runtime values those
constants are compared against.

---

## 4. Base64 Without a Lookup Table

Base64 encoding converts every 3 bytes of input into 4 printable ASCII characters.
It shows up in this project for WebSocket handshakes (`Sec-WebSocket-Key`) and for
encoding compiled shellcode for transport. The standard approach uses a 64-byte lookup
table mapping indices 0-63 to their corresponding characters. That table would live in
`.rodata`, which is exactly what the project cannot have.

The replacement is pure arithmetic. The encoder in `base64.cc` maps a 6-bit value to
its Base64 character using branchless offset computation:

```cpp
static constexpr CHAR Base64EncodeChar(UINT32 v)
{
    v &= 63u;
    return (CHAR)(
        'A' + v +
        (v >= 26u) * ('a' - 'A' - 26) +
        (v >= 52u) * ('0' - 'a' - 26) +
        (v >= 62u) * ('+' - '0' - 10) +
        (v >= 63u) * ('/' - '+' - 1));
}
```

This is worth stepping through. The base expression is `'A' + v`, which is correct
for indices 0-25 (the uppercase letters). Each subsequent term is a conditional
correction. `(v >= 26u)` evaluates to 0 or 1 as an integer. When `v` is 26 or above,
the first correction adjusts the offset so the result lands on lowercase letters.
When `v` hits 52, the second correction redirects to digits. The last two handle `+`
and `/`. No branches, no table, no data section. Every comparison becomes a subtraction
and conditional move at the machine level.

The decoder reverses the mapping with straightforward range checks, returning `0xFF`
for characters outside the Base64 alphabet so callers can detect malformed input
without a separate error flag.

The encoding loop processes input in 3-byte (24-bit) groups. Three bytes are
packed into a single `UINT32`, then four 6-bit slices are extracted with shifts and
masks:

```cpp
UINT32 v =
    ((UINT32)(UINT8)input[i]     << 16) |
    ((UINT32)(UINT8)input[i + 1] <<  8) |
    ((UINT32)(UINT8)input[i + 2]);

output[o++] = Base64EncodeChar((v >> 18) & 63u);
output[o++] = Base64EncodeChar((v >> 12) & 63u);
output[o++] = Base64EncodeChar((v >>  6) & 63u);
output[o++] = Base64EncodeChar( v        & 63u);
```

When the input length is not a multiple of 3, the tail is padded with `=` characters
per RFC 4648 Section 3.5: one remaining byte produces two encoded characters plus `==`,
two remaining bytes produce three encoded characters plus `=`.

---

## 5. xorshift64 -- Pseudorandomness Without libc

`rand()` is gone with `-nostdlib`. The replacement is xorshift64, an algorithm by
George Marsaglia published in 2003. The entire state is a single `UINT64`, and the
update step is three XOR-shift operations:

```cpp
constexpr FORCE_INLINE INT32 Get()
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<INT32>(state & 0x7FFFFFFF);
}
```

Three lines of arithmetic, no external dependencies, period of 2^64 - 1. That last
number means the generator visits every possible non-zero 64-bit state exactly once
before repeating. The return value is masked to 31 bits (the `& 0x7FFFFFFF`) to
produce a non-negative `INT32` in the range [0, 0x7FFFFFFF].

This is emphatically not cryptographically secure. If you observe enough output values,
you can reconstruct the internal state and predict all future output. That is acceptable
here because xorshift64 is only used for things like generating random file names and
WebSocket masking keys -- contexts where unpredictability matters less than simplicity
and the absence of library dependencies.

The generator is seeded from hardware timestamp counters (RDTSC on x86, equivalent on
ARM) via the platform layer's `Random` class, which wraps `Prng`. The seed must be
non-zero -- a zero state produces zero forever.

The `Prng` class also deletes its heap allocation operators (`operator new` and
`operator delete`), forcing stack-only allocation. This is consistent with the
project's approach to memory management described in [06-memory-and-strings.md](06-memory-and-strings.md).

---

## 6. Division-Free Random Letter Generation

Generating a random lowercase letter seems trivial: `'a' + (rand() % 26)`. But
modulo involves integer division, and on some architectures division is expensive --
ARM Cortex-M cores, for instance, may not have a hardware divider at all. The project
sidesteps the issue entirely:

```cpp
template <typename TChar>
constexpr FORCE_INLINE TChar GetChar()
{
    INT32 val = Get();
    INT32 charOffset = ((val & 0x7FFF) * 26) >> 15;
    if (charOffset > 25)
        charOffset = 25;
    return (TChar)('a' + charOffset);
}
```

The trick is fixed-point scaling. `val & 0x7FFF` masks the random value to 15 bits,
giving a range of [0, 32767]. Multiplying by 26 gives [0, 851842]. Right-shifting by
15 is the same as dividing by 32768, giving a result in [0, 25]. The entire operation
compiles down to a bitwise AND, a multiply, and a shift -- no division instruction
anywhere. The guard `if (charOffset > 25)` handles the edge case where rounding might
push the result to 26, clamping it back to 'z'.

This is a classic embedded programming pattern. Anywhere you see `(x * N) >> K` being
used instead of `x % N`, the same idea is at work: approximate modular reduction with
multiply-and-shift, trading a tiny bias in distribution for a significant speedup on
division-limited hardware.

---

## 7. Byte Swapping and Endianness

Network protocols -- TCP, DNS, TLS, HTTP -- use big-endian byte order (most significant
byte first), a convention defined in RFC 1700 as "network byte order." x86 and default
ARM are little-endian (least significant byte first). When an x86 machine stores port
443 as the 16-bit value 0x01BB, the bytes sit in memory as [BB, 01]. The network
expects [01, BB]. Something has to flip them.

Normally you would call `htons()` or `htonl()` from the socket library. Without libc,
those are gone. The `ByteOrder` class provides the replacement:

```cpp
static constexpr FORCE_INLINE UINT16 Swap16(UINT16 value) noexcept
{
    return __builtin_bswap16(value);
}

static constexpr FORCE_INLINE UINT32 Swap32(UINT32 value) noexcept
{
    return __builtin_bswap32(value);
}

static constexpr FORCE_INLINE UINT64 Swap64(UINT64 value) noexcept
{
    return __builtin_bswap64(value);
}
```

Each function wraps a compiler builtin that maps to a single machine instruction:
`BSWAP` on x86, `REV` on ARM. On RISC-V, which lacks a dedicated byte-swap
instruction, the compiler synthesizes an equivalent sequence of shifts and ORs.

The functions are `constexpr`, so the compiler can fold byte swaps of compile-time
constants directly into the instruction stream -- `ByteOrder::Swap16(8080)` does not
emit a `BSWAP`; it just encodes the already-swapped constant. At runtime, these show
up everywhere there is network I/O: port numbers, IP addresses, protocol headers, TLS
record lengths -- any multi-byte field crossing the host/network boundary.

---

## 8. Bit Rotation for Cryptographic Primitives

Bit rotation is a circular shift. Unlike a regular shift where bits fall off one end
and zeros fill in from the other, rotation wraps the displaced bits around. Shift
0xABCD1234 right by 8 and you get 0x00ABCD12 -- the bottom byte is gone. Rotate right
by 8 and you get 0x34ABCD12 -- the bottom byte reappears at the top.

The `BitOps` class provides templated rotation for any unsigned integer type:

```cpp
template<typename T>
static constexpr FORCE_INLINE T Rotr(T x, UINT32 n)
{
    constexpr UINT32 bits = sizeof(T) * 8;
    n &= (bits - 1);
    if (n == 0)
        return x;
    return (x >> n) | (x << (bits - n));
}
```

The pattern `(x >> n) | (x << (bits - n))` is the textbook definition of right
rotation. Every modern compiler recognizes this idiom and emits a single `ROR`
instruction on x86 or ARM rather than actually performing two shifts and an OR.
The `n &= (bits - 1)` masks the rotation amount to avoid undefined behavior from
shifting by the full width of the type.

Left rotation (`Rotl`) is the mirror image:

```cpp
return (x << n) | (x >> (bits - n));
```

These rotations are not incidental utility functions. They are fundamental building
blocks of the cryptographic algorithms used throughout the project:

- **SHA-256** defines its Sigma and sigma functions in terms of 32-bit rotations.
  The compression function's round step uses `Rotr(x, 2)`, `Rotr(x, 13)`,
  `Rotr(x, 22)`, `Rotr(x, 6)`, `Rotr(x, 11)`, and `Rotr(x, 25)`, among others.
- **SHA-512** uses the same patterns but with 64-bit rotations and different
  rotation amounts.
- **ChaCha20** builds its quarter-round function from 32-bit left rotations by
  16, 12, 8, and 7 bits.

Without these single-instruction rotations, every round of SHA-256 would require
twice as many shift and OR instructions, and the crypto code would be both larger
and slower. The templated design means the same `BitOps::Rotr<UINT32>` call serves
SHA-256, while `BitOps::Rotr<UINT64>` serves SHA-512, with no code duplication.

---

## 9. The Common Thread

Every algorithm and math utility in this document exists because the standard library
does not. But the replacements are not just adequate substitutes -- several are
genuinely better suited to the constraints:

- DJB2 with a date-derived seed does not merely avoid strings in the binary; it
  defeats signature-based detection of hash constants across builds.
- The branchless Base64 encoder does not merely avoid a lookup table; it produces
  code that is friendly to the instruction cache and free of data-dependent branches.
- xorshift64 does not merely replace `rand()`; it does so with a smaller footprint
  and a longer period than many libc implementations.
- Compiler builtins for byte swapping do not merely replace `htons()`; they guarantee
  single-instruction codegen without relying on platform headers.

The pattern repeats across the codebase. Constraints that initially look like
handicaps -- no data sections, no standard library, no runtime support -- push the
design toward solutions that are smaller, faster, and harder to fingerprint than
their conventional counterparts. That is not an accident. It is the core engineering
philosophy described in [01-what-is-pic.md](01-what-is-pic.md), applied one algorithm
at a time.
