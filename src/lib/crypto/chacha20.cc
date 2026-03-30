/**
 * @file chacha20.cc
 * @brief ChaCha20-Poly1305 AEAD cipher implementation
 *
 * @details Position-independent implementation of the ChaCha20 stream cipher
 * and Poly1305 message authentication code, as specified in RFC 8439.
 *
 * @note Original ChaCha20 implementation by D. J. Bernstein, public domain.
 *
 * @see RFC 8439 — ChaCha20 and Poly1305 for IETF Protocols
 *      https://datatracker.ietf.org/doc/html/rfc8439
 */

#include "lib/crypto/chacha20.h"
#include "core/memory/memory.h"
#include "platform/platform.h"
#include "platform/console/logger.h"
#include "core/math/bitops.h"

static constexpr FORCE_INLINE UINT32 U8To32Little(const UINT8 *p)
{
	return ((UINT32)(p[0])) | ((UINT32)(p[1]) << 8) | ((UINT32)(p[2]) << 16) | ((UINT32)(p[3]) << 24);
}

static constexpr FORCE_INLINE VOID U32To8Little(UINT8 *p, UINT32 v)
{
	p[0] = (UINT8)(v & 0xFF);
	p[1] = (UINT8)((v >> 8) & 0xFF);
	p[2] = (UINT8)((v >> 16) & 0xFF);
	p[3] = (UINT8)((v >> 24) & 0xFF);
}

static constexpr FORCE_INLINE UINT32 Plus(UINT32 v, UINT32 w)
{
	return (UINT32)((v) + (w)) & 0xFFFFFFFF;
}

static constexpr FORCE_INLINE VOID QuarterRound(UINT32 &a, UINT32 &b, UINT32 &c, UINT32 &d)
{
	a = Plus(a, b); d = BitOps::Rotl32(d ^ a, 16);
	c = Plus(c, d); b = BitOps::Rotl32(b ^ c, 12);
	a = Plus(a, b); d = BitOps::Rotl32(d ^ a, 8);
	c = Plus(c, d); b = BitOps::Rotl32(b ^ c, 7);
}

// --- Poly1305 Implementation ---

Poly1305::Poly1305(const UCHAR (&key)[32])
{
	/// r &= 0xffffffc0ffffffc0ffffffc0fffffff (RFC 8439 Section 2.5)
	r[0] = (U8To32(&key[0])) & 0x3ffffff;
	r[1] = (U8To32(&key[3]) >> 2) & 0x3ffff03;
	r[2] = (U8To32(&key[6]) >> 4) & 0x3ffc0ff;
	r[3] = (U8To32(&key[9]) >> 6) & 0x3f03fff;
	r[4] = (U8To32(&key[12]) >> 8) & 0x00fffff;

	/// h = 0
	Memory::Zero(h, sizeof(h));

	/// Save pad for later
	pad[0] = U8To32(&key[16]);
	pad[1] = U8To32(&key[20]);
	pad[2] = U8To32(&key[24]);
	pad[3] = U8To32(&key[28]);

	leftover = 0;
	finished = 0;
}

Poly1305::~Poly1305()
{
	/// Zero out sensitive state
	Memory::Zero(h, sizeof(h));
	Memory::Zero(r, sizeof(r));
	Memory::Zero(pad, sizeof(pad));
	Memory::Zero(buffer, sizeof(buffer));
	leftover = 0;
	finished = 0;
}

constexpr UINT32 Poly1305::U8To32(const UCHAR *p)
{
	return (((UINT32)(p[0] & 0xff)) |
			((UINT32)(p[1] & 0xff) << 8) |
			((UINT32)(p[2] & 0xff) << 16) |
			((UINT32)(p[3] & 0xff) << 24));
}

/// Stores a 32-bit unsigned integer as four 8-bit unsigned integers in little endian
constexpr VOID Poly1305::U32To8(PUCHAR p, UINT32 v)
{
	p[0] = (v) & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

VOID Poly1305::ProcessBlocks(Span<const UCHAR> data)
{
	const UINT32 hibit = (finished) ? 0 : (1UL << 24); /// 1 << 128
	UINT32 r0, r1, r2, r3, r4;
	UINT32 s1, s2, s3, s4;
	UINT32 h0, h1, h2, h3, h4;
	UINT64 d0, d1, d2, d3, d4;
	UINT32 c;

	const UCHAR *p = data.Data();
	USIZE bytes = data.Size();

	r0 = r[0];
	r1 = r[1];
	r2 = r[2];
	r3 = r[3];
	r4 = r[4];

	s1 = r1 * 5;
	s2 = r2 * 5;
	s3 = r3 * 5;
	s4 = r4 * 5;

	h0 = h[0];
	h1 = h[1];
	h2 = h[2];
	h3 = h[3];
	h4 = h[4];

	while (bytes >= POLY1305_BLOCK_SIZE)
	{
		/// h += m[i]
		h0 += (U8To32(p + 0)) & 0x3ffffff;
		h1 += (U8To32(p + 3) >> 2) & 0x3ffffff;
		h2 += (U8To32(p + 6) >> 4) & 0x3ffffff;
		h3 += (U8To32(p + 9) >> 6) & 0x3ffffff;
		h4 += (U8To32(p + 12) >> 8) | hibit;

		/// h *= r
		d0 = ((UINT64)h0 * r0) + ((UINT64)h1 * s4) + ((UINT64)h2 * s3) + ((UINT64)h3 * s2) + ((UINT64)h4 * s1);
		d1 = ((UINT64)h0 * r1) + ((UINT64)h1 * r0) + ((UINT64)h2 * s4) + ((UINT64)h3 * s3) + ((UINT64)h4 * s2);
		d2 = ((UINT64)h0 * r2) + ((UINT64)h1 * r1) + ((UINT64)h2 * r0) + ((UINT64)h3 * s4) + ((UINT64)h4 * s3);
		d3 = ((UINT64)h0 * r3) + ((UINT64)h1 * r2) + ((UINT64)h2 * r1) + ((UINT64)h3 * r0) + ((UINT64)h4 * s4);
		d4 = ((UINT64)h0 * r4) + ((UINT64)h1 * r3) + ((UINT64)h2 * r2) + ((UINT64)h3 * r1) + ((UINT64)h4 * r0);

		/// (partial) h %= p
		c = (UINT32)(d0 >> 26);
		h0 = (UINT32)d0 & 0x3ffffff;
		d1 += c;
		c = (UINT32)(d1 >> 26);
		h1 = (UINT32)d1 & 0x3ffffff;
		d2 += c;
		c = (UINT32)(d2 >> 26);
		h2 = (UINT32)d2 & 0x3ffffff;
		d3 += c;
		c = (UINT32)(d3 >> 26);
		h3 = (UINT32)d3 & 0x3ffffff;
		d4 += c;
		c = (UINT32)(d4 >> 26);
		h4 = (UINT32)d4 & 0x3ffffff;
		h0 += c * 5;
		c = (h0 >> 26);
		h0 = h0 & 0x3ffffff;
		h1 += c;

		p += POLY1305_BLOCK_SIZE;
		bytes -= POLY1305_BLOCK_SIZE;
	}

	this->h[0] = h0;
	this->h[1] = h1;
	this->h[2] = h2;
	this->h[3] = h3;
	this->h[4] = h4;
}

Result<VOID, Error> Poly1305::GenerateKey(Span<const UCHAR, POLY1305_KEYLEN> key256, Span<const UCHAR> nonce, Span<UCHAR, POLY1305_KEYLEN> polyKey, UINT32 counter)
{
	ChaCha20Poly1305 ctx;
	UINT64 ctr;
	auto keyResult = ctx.KeySetup(key256);
	if (!keyResult)
		return Result<VOID, Error>::Err(keyResult, Error::ChaCha20_GenerateKeyFailed);

	if (nonce.Size() == 8)
	{
		ctr = counter;
		ctx.IVSetup(nonce.Data(), (PUCHAR)&ctr);
	}
	else if (nonce.Size() == 12)
	{
		ctx.IVSetup96BitNonce(nonce.Data(), (PUCHAR)&counter);
	}
	else
	{
		return Result<VOID, Error>::Err(Error::ChaCha20_GenerateKeyFailed);
	}

	ctx.Block(polyKey);
	return Result<VOID, Error>::Ok();
}

VOID Poly1305::Update(Span<const UCHAR> data)
{
	const UCHAR *p = data.Data();
	USIZE bytes = data.Size();
	/// Handle leftover
	if (leftover)
	{
		USIZE want = (POLY1305_BLOCK_SIZE - leftover);
		if (want > bytes)
			want = bytes;
		Memory::Copy(buffer + leftover, p, want);
		bytes -= want;
		p += want;
		leftover += want;
		if (leftover < POLY1305_BLOCK_SIZE)
			return;
		ProcessBlocks(Span<const UCHAR>(buffer, POLY1305_BLOCK_SIZE));
		leftover = 0;
	}

	/// Process full blocks
	if (bytes >= POLY1305_BLOCK_SIZE)
	{
		USIZE want = (bytes & ~(POLY1305_BLOCK_SIZE - 1));
		ProcessBlocks(Span<const UCHAR>(p, want));
		p += want;
		bytes -= want;
	}

	/// Store leftover
	if (bytes)
	{
		Memory::Copy(buffer + leftover, p, bytes);
		leftover += bytes;
	}
}

VOID Poly1305::Finish(Span<UCHAR, POLY1305_TAGLEN> mac)
{
	UINT32 h0, h1, h2, h3, h4, c;
	UINT32 g0, g1, g2, g3, g4;
	UINT64 f;
	UINT32 mask;

	/// Process the remaining block
	if (leftover)
	{
		USIZE i = leftover;
		buffer[i++] = 1;
		Memory::Zero(&buffer[i], POLY1305_BLOCK_SIZE - i);
		finished = 1;
		ProcessBlocks(Span<const UCHAR>(buffer, POLY1305_BLOCK_SIZE));
	}

	/// Fully carry h
	h0 = this->h[0];
	h1 = this->h[1];
	h2 = this->h[2];
	h3 = this->h[3];
	h4 = this->h[4];

	c = h1 >> 26;
	h1 = h1 & 0x3ffffff;
	h2 += c;
	c = h2 >> 26;
	h2 = h2 & 0x3ffffff;
	h3 += c;
	c = h3 >> 26;
	h3 = h3 & 0x3ffffff;
	h4 += c;
	c = h4 >> 26;
	h4 = h4 & 0x3ffffff;
	h0 += c * 5;
	c = h0 >> 26;
	h0 = h0 & 0x3ffffff;
	h1 += c;

	/// Compute h + -p
	g0 = h0 + 5;
	c = g0 >> 26;
	g0 &= 0x3ffffff;
	g1 = h1 + c;
	c = g1 >> 26;
	g1 &= 0x3ffffff;
	g2 = h2 + c;
	c = g2 >> 26;
	g2 &= 0x3ffffff;
	g3 = h3 + c;
	c = g3 >> 26;
	g3 &= 0x3ffffff;
	g4 = h4 + c - (1UL << 26);

	/// Select h if h < p, or h + -p if h >= p
	mask = (g4 >> ((sizeof(UINT32) * 8) - 1)) - 1;
	g0 &= mask;
	g1 &= mask;
	g2 &= mask;
	g3 &= mask;
	g4 &= mask;
	mask = ~mask;
	h0 = (h0 & mask) | g0;
	h1 = (h1 & mask) | g1;
	h2 = (h2 & mask) | g2;
	h3 = (h3 & mask) | g3;
	h4 = (h4 & mask) | g4;

	/// h = h % (2^128)
	h0 = ((h0) | (h1 << 26)) & 0xffffffff;
	h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
	h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
	h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;

	/// mac = (h + pad) % (2^128)
	f = (UINT64)h0 + pad[0];
	h0 = (UINT32)f;
	f = (UINT64)h1 + pad[1] + (f >> 32);
	h1 = (UINT32)f;
	f = (UINT64)h2 + pad[2] + (f >> 32);
	h2 = (UINT32)f;
	f = (UINT64)h3 + pad[3] + (f >> 32);
	h3 = (UINT32)f;

	U32To8(mac.Data() + 0, h0);
	U32To8(mac.Data() + 4, h1);
	U32To8(mac.Data() + 8, h2);
	U32To8(mac.Data() + 12, h3);

	/// Zero out the state
	Memory::Zero(this->h, sizeof(this->h));
	Memory::Zero(this->r, sizeof(this->r));
	Memory::Zero(this->pad, sizeof(this->pad));
}

// --- ChaCha20 Implementation (D. J. Bernstein, https://cr.yp.to/chacha.html) ---

Result<VOID, Error> ChaCha20Poly1305::KeySetup(Span<const UINT8> key)
{
	UINT32 keyBits = (UINT32)key.Size() * 8;
	if (keyBits != 128 && keyBits != 256)
		return Result<VOID, Error>::Err(Error::ChaCha20_KeySetupFailed);

	const UINT8 *k = key.Data();

	this->state[4] = U8To32Little(k + 0);
	this->state[5] = U8To32Little(k + 4);
	this->state[6] = U8To32Little(k + 8);
	this->state[7] = U8To32Little(k + 12);
	// Declare strings separately to avoid type deduction issues with ternary
	auto constants32 = "expand 32-byte k";
	auto constants16 = "expand 16-byte k";
	const CHAR *constants = keyBits == 256 ? (const CHAR *)constants32 : (const CHAR *)constants16;
	if (keyBits == 256)
	{
		/// 256-bit key recommended (RFC 8439 Section 2.3)
		k += 16;
	}
	this->state[8] = U8To32Little(k + 0);
	this->state[9] = U8To32Little(k + 4);
	this->state[10] = U8To32Little(k + 8);
	this->state[11] = U8To32Little(k + 12);
	this->state[0] = U8To32Little((const UINT8 *)constants + 0);
	this->state[1] = U8To32Little((const UINT8 *)constants + 4);
	this->state[2] = U8To32Little((const UINT8 *)constants + 8);
	this->state[3] = U8To32Little((const UINT8 *)constants + 12);
	return Result<VOID, Error>::Ok();
}

VOID ChaCha20Poly1305::Key(UINT8 (&k)[32])
{
	U32To8Little(k, this->state[4]);
	U32To8Little(k + 4, this->state[5]);
	U32To8Little(k + 8, this->state[6]);
	U32To8Little(k + 12, this->state[7]);

	U32To8Little(k + 16, this->state[8]);
	U32To8Little(k + 20, this->state[9]);
	U32To8Little(k + 24, this->state[10]);
	U32To8Little(k + 28, this->state[11]);
}

VOID ChaCha20Poly1305::Nonce(UINT8 (&nonce)[TLS_CHACHA20_IV_LENGTH])
{
	U32To8Little(nonce + 0, this->state[13]);
	U32To8Little(nonce + 4, this->state[14]);
	U32To8Little(nonce + 8, this->state[15]);
}

VOID ChaCha20Poly1305::IVSetup(const UINT8 *iv, const UINT8 *counter)
{
	this->state[12] = counter == nullptr ? 0 : U8To32Little(counter + 0);
	this->state[13] = counter == nullptr ? 0 : U8To32Little(counter + 4);
	if (iv)
	{
		this->state[14] = U8To32Little(iv + 0);
		this->state[15] = U8To32Little(iv + 4);
	}
}

VOID ChaCha20Poly1305::IVSetup96BitNonce(const UINT8 *iv, const UINT8 *counter)
{
	this->state[12] = counter == nullptr ? 0 : U8To32Little(counter + 0);
	if (iv)
	{
		this->state[13] = U8To32Little(iv + 0);
		this->state[14] = U8To32Little(iv + 4);
		this->state[15] = U8To32Little(iv + 8);
	}
}

VOID ChaCha20Poly1305::IVUpdate(Span<const UINT8, TLS_CHACHA20_IV_LENGTH> iv, Span<const UINT8, 8> aad, const UINT8 *counter)
{
	this->state[12] = counter == nullptr ? 0 : U8To32Little(counter + 0);
	this->state[13] = U8To32Little(iv.Data() + 0);
	this->state[14] = U8To32Little(iv.Data() + 4) ^ U8To32Little(aad.Data());
	this->state[15] = U8To32Little(iv.Data() + 8) ^ U8To32Little(aad.Data() + 4);
}

VOID ChaCha20Poly1305::EncryptBytes(Span<const UINT8> plaintext, Span<UINT8> output)
{
	const UINT8 *m = plaintext.Data();
	UINT8 *c = output.Data();
	UINT32 bytes = (UINT32)plaintext.Size();

	UINT32 x0, x1, x2, x3, x4, x5, x6, x7;
	UINT32 x8, x9, x10, x11, x12, x13, x14, x15;
	UINT32 j0, j1, j2, j3, j4, j5, j6, j7;
	UINT32 j8, j9, j10, j11, j12, j13, j14, j15;
	UINT8 *ctarget = nullptr;
	UINT8 tmp[64];
	UINT32 i;

	if (!bytes)
		return;

	j0 = this->state[0];
	j1 = this->state[1];
	j2 = this->state[2];
	j3 = this->state[3];
	j4 = this->state[4];
	j5 = this->state[5];
	j6 = this->state[6];
	j7 = this->state[7];
	j8 = this->state[8];
	j9 = this->state[9];
	j10 = this->state[10];
	j11 = this->state[11];
	j12 = this->state[12];
	j13 = this->state[13];
	j14 = this->state[14];
	j15 = this->state[15];

	for (;;)
	{
		if (bytes < CHACHA_BLOCKLEN)
		{
			Memory::Copy(tmp, m, bytes);
			m = tmp;
			ctarget = c;
			c = tmp;
		}
		x0 = j0;
		x1 = j1;
		x2 = j2;
		x3 = j3;
		x4 = j4;
		x5 = j5;
		x6 = j6;
		x7 = j7;
		x8 = j8;
		x9 = j9;
		x10 = j10;
		x11 = j11;
		x12 = j12;
		x13 = j13;
		x14 = j14;
		x15 = j15;
		for (i = 20; i > 0; i -= 2)
		{
			QuarterRound(x0, x4, x8, x12);
			QuarterRound(x1, x5, x9, x13);
			QuarterRound(x2, x6, x10, x14);
			QuarterRound(x3, x7, x11, x15);
			QuarterRound(x0, x5, x10, x15);
			QuarterRound(x1, x6, x11, x12);
			QuarterRound(x2, x7, x8, x13);
			QuarterRound(x3, x4, x9, x14);
		}
		x0 = Plus(x0, j0);
		x1 = Plus(x1, j1);
		x2 = Plus(x2, j2);
		x3 = Plus(x3, j3);
		x4 = Plus(x4, j4);
		x5 = Plus(x5, j5);
		x6 = Plus(x6, j6);
		x7 = Plus(x7, j7);
		x8 = Plus(x8, j8);
		x9 = Plus(x9, j9);
		x10 = Plus(x10, j10);
		x11 = Plus(x11, j11);
		x12 = Plus(x12, j12);
		x13 = Plus(x13, j13);
		x14 = Plus(x14, j14);
		x15 = Plus(x15, j15);

		if (bytes < CHACHA_BLOCKLEN)
		{
			U32To8Little(this->ks + 0, x0);
			U32To8Little(this->ks + 4, x1);
			U32To8Little(this->ks + 8, x2);
			U32To8Little(this->ks + 12, x3);
			U32To8Little(this->ks + 16, x4);
			U32To8Little(this->ks + 20, x5);
			U32To8Little(this->ks + 24, x6);
			U32To8Little(this->ks + 28, x7);
			U32To8Little(this->ks + 32, x8);
			U32To8Little(this->ks + 36, x9);
			U32To8Little(this->ks + 40, x10);
			U32To8Little(this->ks + 44, x11);
			U32To8Little(this->ks + 48, x12);
			U32To8Little(this->ks + 52, x13);
			U32To8Little(this->ks + 56, x14);
			U32To8Little(this->ks + 60, x15);
		}

		x0 ^= U8To32Little(m + 0);
		x1 ^= U8To32Little(m + 4);
		x2 ^= U8To32Little(m + 8);
		x3 ^= U8To32Little(m + 12);
		x4 ^= U8To32Little(m + 16);
		x5 ^= U8To32Little(m + 20);
		x6 ^= U8To32Little(m + 24);
		x7 ^= U8To32Little(m + 28);
		x8 ^= U8To32Little(m + 32);
		x9 ^= U8To32Little(m + 36);
		x10 ^= U8To32Little(m + 40);
		x11 ^= U8To32Little(m + 44);
		x12 ^= U8To32Little(m + 48);
		x13 ^= U8To32Little(m + 52);
		x14 ^= U8To32Little(m + 56);
		x15 ^= U8To32Little(m + 60);

		j12 = Plus(j12, 1);
		if (!j12)
		{
			j13 = Plus(j13, 1);
			/// Stopping at 2^70 bytes per nonce is the user's responsibility
		}

		U32To8Little(c + 0, x0);
		U32To8Little(c + 4, x1);
		U32To8Little(c + 8, x2);
		U32To8Little(c + 12, x3);
		U32To8Little(c + 16, x4);
		U32To8Little(c + 20, x5);
		U32To8Little(c + 24, x6);
		U32To8Little(c + 28, x7);
		U32To8Little(c + 32, x8);
		U32To8Little(c + 36, x9);
		U32To8Little(c + 40, x10);
		U32To8Little(c + 44, x11);
		U32To8Little(c + 48, x12);
		U32To8Little(c + 52, x13);
		U32To8Little(c + 56, x14);
		U32To8Little(c + 60, x15);

		if (bytes <= CHACHA_BLOCKLEN)
		{
			if (bytes < CHACHA_BLOCKLEN)
			{
				Memory::Copy(ctarget, c, bytes);
			}
			this->state[12] = j12;
			this->state[13] = j13;
			this->unused = CHACHA_BLOCKLEN - bytes;
			return;
		}
		bytes -= CHACHA_BLOCKLEN;
		c += CHACHA_BLOCKLEN;
		m += CHACHA_BLOCKLEN;
	}
}

VOID ChaCha20Poly1305::Block(Span<UCHAR> output)
{
	UINT32 i;
	PUCHAR c = output.Data();

	UINT32 blkState[16];
	for (i = 0; i < 16; i++)
		blkState[i] = this->state[i];
	for (i = 20; i > 0; i -= 2)
	{
		QuarterRound(blkState[0], blkState[4], blkState[8], blkState[12]);
		QuarterRound(blkState[1], blkState[5], blkState[9], blkState[13]);
		QuarterRound(blkState[2], blkState[6], blkState[10], blkState[14]);
		QuarterRound(blkState[3], blkState[7], blkState[11], blkState[15]);
		QuarterRound(blkState[0], blkState[5], blkState[10], blkState[15]);
		QuarterRound(blkState[1], blkState[6], blkState[11], blkState[12]);
		QuarterRound(blkState[2], blkState[7], blkState[8], blkState[13]);
		QuarterRound(blkState[3], blkState[4], blkState[9], blkState[14]);
	}

	for (i = 0; i < 16; i++)
		blkState[i] = Plus(blkState[i], this->state[i]);

	for (i = 0; i < (UINT32)output.Size(); i += 4)
	{
		U32To8Little(c + i, blkState[i / 4]);
	}
	this->state[12] = Plus(this->state[12], 1);
}

NOINLINE VOID ChaCha20Poly1305::Poly1305PadAndTrail(Poly1305 &poly, Span<const UCHAR> aad, Span<const UCHAR> ciphertext)
{
	UCHAR zeropad[15];
	Memory::Zero(zeropad, sizeof(zeropad));

	poly.Update(aad);
	INT32 rem = (INT32)aad.Size() % 16;
	if (rem)
		poly.Update(Span<const UCHAR>(zeropad, 16 - rem));
	poly.Update(ciphertext);
	rem = (INT32)ciphertext.Size() % 16;
	if (rem)
		poly.Update(Span<const UCHAR>(zeropad, 16 - rem));

	UCHAR trail[16];
	Poly1305::U32To8(trail, (UINT32)aad.Size());
	Memory::Zero(trail + 4, 4);
	Poly1305::U32To8(trail + 8, (UINT32)ciphertext.Size());
	Memory::Zero(trail + 12, 4);
	poly.Update(Span<const UCHAR>(trail));
}

VOID ChaCha20Poly1305::Poly1305Aead(Span<const UCHAR> pt, Span<const UCHAR> aad, const UCHAR (&polyKey)[POLY1305_KEYLEN], Span<UCHAR> out)
{
	UINT32 counter = 1;
	this->IVSetup96BitNonce(nullptr, (PUCHAR)&counter);
	this->EncryptBytes(Span<const UINT8>(pt.Data(), pt.Size()), Span<UINT8>(out.Data(), pt.Size()));

	Poly1305 poly(polyKey);
	ChaCha20Poly1305::Poly1305PadAndTrail(poly, aad, Span<const UCHAR>(out.Data(), pt.Size()));
	poly.Finish(out.Last<POLY1305_TAGLEN>());
}

Result<INT32, Error> ChaCha20Poly1305::Poly1305Decode(Span<const UCHAR> pt, Span<const UCHAR> aad, const UCHAR (&polyKey)[POLY1305_KEYLEN], Span<UCHAR> out)
{
	if (pt.Size() < POLY1305_TAGLEN)
		return Result<INT32, Error>::Err(Error::ChaCha20_DecodeFailed);

	UINT32 len = (UINT32)pt.Size() - POLY1305_TAGLEN;

	// Authenticate BEFORE decrypting (AEAD requirement)
	// polyKey is already computed by the caller; use it directly
	Poly1305 poly(polyKey);
	ChaCha20Poly1305::Poly1305PadAndTrail(poly, aad, Span<const UCHAR>(pt.Data(), len));

	UCHAR macTag[POLY1305_TAGLEN];
	poly.Finish(macTag);

	// Constant-time comparison to prevent timing oracle
	UINT8 diff = 0;
	for (UINT32 i = 0; i < POLY1305_TAGLEN; i++)
		diff |= macTag[i] ^ pt[len + i];

	if (diff != 0)
	{
		LOG_ERROR("ChaCha20Poly1305::Poly1305Decode: Authentication tag mismatch");
		Memory::Zero(macTag, sizeof(macTag));
		return Result<INT32, Error>::Err(Error::ChaCha20_DecodeFailed);
	}

	// Only decrypt after authentication succeeds
	this->EncryptBytes(Span<const UINT8>(pt.Data(), len), Span<UINT8>(out.Data(), len));

	Memory::Zero(macTag, sizeof(macTag));
	return Result<INT32, Error>::Ok((INT32)len);
}

VOID ChaCha20Poly1305::Poly1305Key(Span<UCHAR, POLY1305_KEYLEN> polyKey)
{
	UCHAR key[32];
	UCHAR nonce[12];
	this->Key(key);
	this->Nonce(nonce);
	// Nonce is always 12 bytes, so GenerateKey cannot fail
	(VOID)Poly1305::GenerateKey(key, nonce, polyKey, 0);
	Memory::Zero(key, sizeof(key));
	Memory::Zero(nonce, sizeof(nonce));
}

ChaCha20Poly1305::ChaCha20Poly1305()
{
	Memory::Zero(this->state, sizeof(this->state));
	Memory::Zero(this->ks, sizeof(this->ks));
	this->unused = 0;
}

ChaCha20Poly1305::~ChaCha20Poly1305()
{
	Memory::Zero(this->state, sizeof(this->state));
	Memory::Zero(this->ks, sizeof(this->ks));
	this->unused = 0;
}

ChaCha20Poly1305::ChaCha20Poly1305(ChaCha20Poly1305 &&other) noexcept
{
	Memory::Copy(this->state, other.state, sizeof(this->state));
	Memory::Copy(this->ks, other.ks, sizeof(this->ks));
	this->unused = other.unused;
	Memory::Zero(other.state, sizeof(other.state));
	Memory::Zero(other.ks, sizeof(other.ks));
	other.unused = 0;
}

ChaCha20Poly1305 &ChaCha20Poly1305::operator=(ChaCha20Poly1305 &&other) noexcept
{
	if (this != &other)
	{
		Memory::Copy(this->state, other.state, sizeof(this->state));
		Memory::Copy(this->ks, other.ks, sizeof(this->ks));
		this->unused = other.unused;
		Memory::Zero(other.state, sizeof(other.state));
		Memory::Zero(other.ks, sizeof(other.ks));
		other.unused = 0;
	}
	return *this;
}
