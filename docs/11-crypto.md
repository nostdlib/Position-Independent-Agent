# Cryptography: Everything From Scratch

This document explains every cryptographic primitive in the agent's
`src/lib/crypto/` directory -- why they exist, how they work internally, and
what keeps them position-independent. If you have not read
[01-what-is-pic.md](01-what-is-pic.md) and [05-core-types.md](05-core-types.md)
yet, do that first. The constraints described there (no .rdata, no libc, no
global state) are the reason this code exists.

---

## 1. Why Implement Crypto From Scratch

OpenSSL is roughly 500,000 lines of code. It depends on libc, dynamic linking,
and global state. It cannot compile in freestanding mode (`-nostdlib`). Even if
you gutted it enough to compile, its lookup tables would land in `.rodata`,
breaking position independence. Non-starter.

The agent implements only what TLS 1.3 requires:

| Algorithm           | Role                          | Source             |
|---------------------|-------------------------------|--------------------|
| ChaCha20-Poly1305   | Symmetric encryption (AEAD)   | `chacha20.h/.cc`   |
| SHA-256 / SHA-384   | Hashing, HMAC, key derivation | `sha2.h/.cc`       |
| ECDH (P-256, P-384) | Key exchange                  | `ecc.h/.cc`        |
| HKDF                | Key derivation                | `hkdf.h/.cc`       |

Three rules apply to all of it: no lookup tables (constants are immediates in
code or populated on the stack), no global state, and constant-time operations
wherever secrets are involved.

---

## 2. ChaCha20: The Stream Cipher

ChaCha20 generates a pseudorandom keystream and XORs it with plaintext. XOR
again with the same keystream to decrypt -- self-inverse.

### 2.1 The State Matrix

Sixteen 32-bit words arranged as a 4x4 matrix:
```
"expa"  "nd 3"  "2-by"  "te k"    <- constants
 key0    key1    key2    key3      <- 256-bit key (8 words)
 key4    key5    key6    key7
 ctr     nonce0  nonce1  nonce2    <- 32-bit counter + 96-bit nonce
```
The constants are ASCII for `"expand 32-byte k"`. During `KeySetup`:
```cpp
auto constants32 = "expand 32-byte k";
this->state[0] = U8To32Little((const UINT8 *)constants + 0);
this->state[1] = U8To32Little((const UINT8 *)constants + 4);
// ... state[4..11] = key, state[12] = counter, state[13..15] = nonce
```
This layout matches RFC 8439 Section 2.3 exactly.

### 2.2 The Quarter Round

The fundamental mixing operation -- four words in, four words out:
```cpp
static constexpr FORCE_INLINE VOID QuarterRound(
    UINT32 &a, UINT32 &b, UINT32 &c, UINT32 &d)
{
    a = Plus(a, b); d = BitOps::Rotl32(d ^ a, 16);
    c = Plus(c, d); b = BitOps::Rotl32(b ^ c, 12);
    a = Plus(a, b); d = BitOps::Rotl32(d ^ a, 8);
    c = Plus(c, d); b = BitOps::Rotl32(b ^ c, 7);
}
```
Four add-xor-rotate triples. The rotation amounts (16, 12, 8, 7) maximize
diffusion across all 32 bits in the fewest steps.

### 2.3 Twenty Rounds

Ten iterations of 4 column rounds + 4 diagonal rounds:
```cpp
for (i = 20; i > 0; i -= 2)
{
    QuarterRound(x0,  x4,  x8,  x12);   // column rounds
    QuarterRound(x1,  x5,  x9,  x13);
    QuarterRound(x2,  x6,  x10, x14);
    QuarterRound(x3,  x7,  x11, x15);
    QuarterRound(x0,  x5,  x10, x15);   // diagonal rounds
    QuarterRound(x1,  x6,  x11, x12);
    QuarterRound(x2,  x7,  x8,  x13);
    QuarterRound(x3,  x4,  x9,  x14);
}
```
After all rounds, the scrambled state is added back to the original
(`Plus(x0, j0)` etc.). Without this addition the function would be invertible
and the cipher broken. Each block produces 64 bytes of keystream.

---

## 3. Poly1305: The Authenticator

Poly1305 computes a 128-bit MAC. Combined with ChaCha20, it forms the AEAD
scheme: ChaCha20 provides confidentiality, Poly1305 provides integrity.

### 3.1 The Math

Polynomial evaluation modulo 2^130 - 5. Each 16-byte block becomes a
coefficient: `tag = ((m1*r + m2)*r + m3)*r + ...) mod (2^130 - 5) + s`.
The `r` is a clamped multiplier (128 bits); `s` is a secret addend (128 bits).

### 3.2 26-Bit Limbs

A 130-bit number splits into 5 limbs of 26 bits each. Why 26? Multiplying
two 26-bit values gives at most 52 bits, which fits in a 64-bit register.
Full 32-bit limbs would produce 64-bit products and carries would overflow.
The 26-bit choice avoids 128-bit arithmetic entirely.

The `r` array initialization shows clamping and limb splitting together:
```cpp
r[0] = (U8To32(&key[0]))  & 0x3ffffff;
r[1] = (U8To32(&key[3]) >> 2) & 0x3ffff03;
r[2] = (U8To32(&key[6]) >> 4) & 0x3ffc0ff;
r[3] = (U8To32(&key[9]) >> 6) & 0x3f03fff;
r[4] = (U8To32(&key[12]) >> 8) & 0x00fffff;
```
The masks extract 26-bit limbs and enforce clamping simultaneously.

### 3.3 The Multiply-Accumulate Core

For each 16-byte block, add it to accumulator `h` then multiply by `r`:
```cpp
h0 += (U8To32(p + 0)) & 0x3ffffff;
// ... load remaining limbs of block into h1..h4 ...
h4 += (U8To32(p + 12) >> 8) | hibit;

d0 = ((UINT64)h0 * r0) + ((UINT64)h1 * s4) + ((UINT64)h2 * s3)
   + ((UINT64)h3 * s2) + ((UINT64)h4 * s1);
// ... d1 through d4 follow the same pattern ...
```
The `s1..s4` values are `r1*5..r4*5`. This is the reduction trick: since we
work modulo 2^130 - 5, overflow from the top limb wraps and gets multiplied
by 5, keeping everything in 26-bit limbs after carry propagation.

---

## 4. The Counter=1 Rule

RFC 8439 Section 2.6 reserves block 0 for key generation:
1. Run ChaCha20 with counter=0 to generate 64 bytes.
2. Take the first 32 bytes as the one-time Poly1305 key.
3. Start encryption at counter=1.

From `Poly1305Aead`:
```cpp
UINT32 counter = 1;
this->IVSetup96BitNonce(nullptr, (PUCHAR)&counter);
this->EncryptBytes(/* plaintext */, /* output */);
Poly1305 poly(polyKey);   // polyKey was derived from block 0 by caller
```
Every message gets a unique Poly1305 key derived from the ChaCha20 key + nonce
combination. Reusing a Poly1305 key with different messages breaks security.

---

## 5. Constant-Time Comparison

Tag verification must not leak information through timing. A naive early-return
comparison lets an attacker reconstruct the valid tag byte by byte by measuring
how long each attempt takes. The actual code (line 661 of `chacha20.cc`):
```cpp
UINT8 diff = 0;
for (UINT32 i = 0; i < POLY1305_TAGLEN; i++)
    diff |= macTag[i] ^ pt[len + i];
if (diff != 0)
{
    Memory::Zero(macTag, sizeof(macTag));
    return Result<INT32, Error>::Err(Error::ChaCha20_DecodeFailed);
}
```
Every byte is checked. Every iteration runs. The attacker learns nothing from
timing. Note also: the code authenticates before decrypting (AEAD requirement).
If the tag is wrong, ciphertext is never decrypted and no unauthenticated
plaintext is released.

---

## 6. SHA-256 Without Tables

SHA-256 needs 64 round constants `K[]` and 8 initial hash values `H0[]`.
Standard implementations store these in `.rodata`. That breaks PIC. Instead,
`FillH0()` and `FillK()` populate arrays on the stack:
```cpp
VOID SHA256Traits::FillH0(Word (&out)[8])
{
    const UINT32 embedded[] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    Memory::Copy(out, embedded, sizeof(out));
}
```
The `const UINT32 embedded[]` inside the function body compiles into `mov`
instructions or stack writes -- values live in `.text` as part of the
instruction encoding, not in a data section. `NOINLINE` on `Transform` prevents
the compiler from constant-folding these back into global tables.

SHA-256 is big-endian internally, but the agent runs on little-endian x86.
The `Pack` function handles conversion:
```cpp
constexpr VOID SHA256Traits::Pack(Span<const UINT8, sizeof(Word)> str, Word &x)
{
    x = ((Word)str[3]) | ((Word)str[2] << 8)
      | ((Word)str[1] << 16) | ((Word)str[0] << 24);
}
```

---

## 7. HMAC: Keyed Hashing

HMAC combines a secret key with SHA-256 for message authentication. It appears
throughout TLS 1.3 via HKDF. The construction (RFC 2104):
```
HMAC(K, m) = SHA256( (K' XOR opad) || SHA256( (K' XOR ipad) || m ) )
```
Where `ipad = 0x36 * blocksize` and `opad = 0x5C * blocksize`.

The `HMACBase` template stores cached SHA states for efficiency:
```cpp
SHAType ctxInside;         // Inner hash context
SHAType ctxOutside;        // Outer hash context
SHAType ctxInsideReinit;   // Saved state after processing ipad
SHAType ctxOutsideReinit;  // Saved state after processing opad
```
After `Init()` processes the key pads, `Reinit()` can restore the cached state
instead of re-hashing the key. This matters in HKDF-Expand, which computes many
HMACs with the same key in a tight loop.

---

## 8. ECDH: Key Exchange Step by Step

ECDH lets two parties agree on a shared secret over an insecure channel. This
is the foundation of the TLS 1.3 handshake. For the full handshake flow, see
[ARCHITECTURE-MAP.md](ARCHITECTURE-MAP.md).

### 8.1 The Protocol

1. Client picks random private key `a`, computes `A = a * G` (G is the generator).
2. Server picks random private key `b`, computes `B = b * G`.
3. They exchange `A` and `B` in the clear.
4. Client computes `a * B = a*b*G`. Server computes `b * A = b*a*G`. Same point.
5. The x-coordinate of that point is the shared secret.
6. An eavesdropper sees `A` and `B` but cannot recover `a*b*G` without `a` or
   `b` -- that is the Elliptic Curve Discrete Logarithm Problem.

### 8.2 The Code

```cpp
class ECC {
    UINT64 curveP[MAX_NUM_ECC_DIGITS];      // Prime modulus
    UINT64 curveB[MAX_NUM_ECC_DIGITS];      // Curve coefficient b
    ECCPoint curveG;                        // Generator G
    UINT64 curveN[MAX_NUM_ECC_DIGITS];      // Order of G
    UINT64 privateKey[MAX_NUM_ECC_DIGITS];  // Secret scalar d
    ECCPoint publicKey;                     // Q = d * G
};
```
All curve parameters are embedded in code (same trick as SHA-256). Usage:
```cpp
ECC ecdh;
ecdh.Initialize(32);                   // P-256
ecdh.ExportPublicKey(pubKey);          // 0x04 || x || y
ecdh.ComputeSharedSecret(peerPub, secret);  // shared secret -> HKDF
```
VLI (Variable Length Integer) arithmetic implements big-number operations over
`UINT64` arrays, with curve-specific fast reduction (`VliMmodFast256`,
`VliMmodFast384`) that exploits the special structure of NIST primes.

---

## 9. The Montgomery Ladder

Scalar multiplication (`k * G`) is done bit by bit. The naive approach:
```
for each bit of k (high to low):
    R = double(R)
    if bit == 1:  R = add(R, G)
```
When bit is 1, you double-and-add. When 0, you only double. An attacker
measuring power, EM emissions, or cache behavior can read off every bit of
the private key. This is a side-channel attack.

The Montgomery ladder always does the same two operations per bit:
```
R0 = infinity, R1 = G
for each bit of k (high to low):
    if bit == 0:  R1 = add(R0, R1);  R0 = double(R0)
    else:         R0 = add(R0, R1);  R1 = double(R1)
```
One add and one double every iteration, regardless of the bit value. Only which
register gets which result changes. In practice, this is a conditional swap, not
a branch. The implementation uses co-Z point operations (`XYcZAdd`, `XYcZAddC`)
where both points share the same Z coordinate, reducing field multiplications
per step. Same timing for every key. No leakage.

---

## 10. Jacobian Coordinates

In affine coordinates `(x, y)`, point addition requires a modular inverse --
essentially division on a 256-bit field. Modular inversion is orders of
magnitude slower than multiplication.

Jacobian coordinates represent a point as `(X, Y, Z)` where `x = X/Z^2` and
`y = Y/Z^3`. Point operations use only multiplication and squaring:
```cpp
VOID DoubleJacobian(UINT64 (&X1)[MAX_NUM_ECC_DIGITS],
                    UINT64 (&Y1)[MAX_NUM_ECC_DIGITS],
                    UINT64 (&Z1)[MAX_NUM_ECC_DIGITS]);

VOID ApplyZ(UINT64 (&X1)[MAX_NUM_ECC_DIGITS],   // convert back to affine
            UINT64 (&Y1)[MAX_NUM_ECC_DIGITS],    // ONLY place modular
            UINT64 (&Z)[MAX_NUM_ECC_DIGITS]);    // inverse happens
```
Over hundreds of point doublings and additions, avoiding modular inversion at
each step gives roughly 3x speedup. `ApplyZ` is the single call to `VliModInv`
at the very end.

NIST curves use `a = -3` in the curve equation (`y^2 = x^3 - 3x + b`). In
Jacobian form this becomes `Y^2 = X^3 - 3*X*Z^4 + b*Z^6`. The `-3` was chosen
by NIST because it saves one field multiplication per doubling operation.

---

## 11. How It All Chains Together

During a TLS 1.3 handshake:
1. **ECDH** produces the shared secret.
2. **HKDF** (built on **HMAC-SHA256**) derives traffic keys from it.
3. **ChaCha20-Poly1305** encrypts and authenticates application data.
4. **SHA-256** hashes handshake transcripts for Finished messages.

All of it runs without libc, without data-section tables, and without timing
leaks on secret material. The entire crypto stack is roughly 3,000 lines of
C++ -- compare that to OpenSSL's half million.

For overall architecture, see [ARCHITECTURE-MAP.md](ARCHITECTURE-MAP.md). For
the build system, see [03-build-system.md](03-build-system.md). For memory
management without a standard library, see
[06-memory-and-strings.md](06-memory-and-strings.md).
