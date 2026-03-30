/**
 * @file ecc.cc
 * @brief Elliptic Curve Cryptography (ECC) implementation
 *
 * @details Variable-length integer arithmetic, modular operations, and
 * elliptic curve point operations for NIST P-256 and P-384 curves.
 */

#include "lib/crypto/ecc.h"
#include "platform/system/random.h"
#include "platform/platform.h"
#include "core/compiler/compiler_builtins.h"
#include "core/memory/memory.h"

/// Curve selection options
constexpr INT32 SECP256R1 = 32;
constexpr INT32 SECP384R1 = 48;
constexpr INT32 MAX_TRIES = 16;

VOID ECC::VliClear(Span<UINT64> vli)
{
	for (UINT32 i = 0; i < this->numEccDigits; ++i)
		vli[i] = 0;
}

/// Returns 1 if vli == 0, 0 otherwise.
BOOL ECC::VliIsZero(Span<const UINT64> vli)
{
	UINT64 acc = 0;
	for (UINT32 i = 0; i < this->numEccDigits; ++i)
		acc |= vli[i];
	return acc == 0;
}

/// Returns nonzero if bit bit of vli is set.
UINT64 ECC::VliTestBit(Span<const UINT64> vli, UINT32 bit)
{
	return (vli[bit >> 6] & ((UINT64)1 << (bit & 63)));
}

/// Counts the number of 64-bit "digits" in vli.
UINT32 ECC::VliNumDigits(Span<const UINT64> vli)
{
	INT32 i;
	// Search from the end until we find a non-zero digit.
	// We do it in reverse because we expect that most digits will be nonzero.
	for (i = this->numEccDigits - 1; i >= 0 && vli[i] == 0; --i)
	{
	}

	return (i + 1);
}

/// Counts the number of bits required for vli.
UINT32 ECC::VliNumBits(Span<const UINT64> vli)
{
	UINT32 numDigits = this->VliNumDigits(vli);
	if (numDigits == 0)
	{
		return 0;
	}

	UINT64 digit = vli[numDigits - 1];
	return ((numDigits - 1) * 64 + (64 - __builtin_clzll(digit)));
}

/// Sets dest = src.
VOID ECC::VliSet(Span<UINT64> dest, Span<const UINT64> src)
{
	for (UINT32 i = 0; i < this->numEccDigits; ++i)
		dest[i] = src[i];
}

/// Returns sign of left - right.
INT32 ECC::VliCmp(Span<const UINT64> left, Span<const UINT64> right)
{
	INT32 i;
	for (i = this->numEccDigits - 1; i >= 0; --i)
	{
		if (left[i] > right[i])
		{
			return 1;
		}
		else if (left[i] < right[i])
		{
			return -1;
		}
	}
	return 0;
}

/// Computes result = in << c, returning carry. Can modify in place (if result == in). 0 < shift < 64.
UINT64 ECC::VliLShift(Span<UINT64> result, Span<const UINT64> input, UINT32 shift)
{
	UINT64 carry = 0;
	UINT32 i;
	for (i = 0; i < this->numEccDigits; ++i)
	{
		UINT64 temp = input[i];
		result[i] = (temp << shift) | carry;
		carry = temp >> (64 - shift);
	}

	return carry;
}

/// Computes vli = vli >> 1.
VOID ECC::VliRShift1(Span<UINT64> vli)
{
	UINT64 carry = 0;

	for (INT32 i = this->numEccDigits - 1; i >= 0; --i)
	{
		UINT64 temp = vli[i];
		vli[i] = (temp >> 1) | carry;
		carry = temp << 63;
	}
}

/// Computes result = left + right, returning carry. Can modify in place.
UINT64 ECC::VliAdd(Span<UINT64> result, Span<const UINT64> left, Span<const UINT64> right)
{
	UINT64 carry = 0;
	for (UINT32 i = 0; i < this->numEccDigits; ++i)
	{
		result[i] = __builtin_addcll(left[i], right[i], carry, &carry);
	}
	return carry;
}

/// Computes result = left - right, returning borrow. Can modify in place.
UINT64 ECC::VliSub(Span<UINT64> result, Span<const UINT64> left, Span<const UINT64> right)
{
	UINT64 borrow = 0;
	for (UINT32 i = 0; i < this->numEccDigits; ++i)
	{
		result[i] = __builtin_subcll(left[i], right[i], borrow, &borrow);
	}
	return borrow;
}

constexpr UInt128 ECC::Mul64_64(UINT64 left, UINT64 right)
{
	UINT32 a0 = (UINT32)left;
	UINT32 a1 = (UINT32)(left >> 32);
	UINT32 b0 = (UINT32)right;
	UINT32 b1 = (UINT32)(right >> 32);

	UINT64 m0 = (UINT64)a0 * b0;
	UINT64 m1 = (UINT64)a0 * b1;
	UINT64 m2 = (UINT64)a1 * b0;
	UINT64 m3 = (UINT64)a1 * b1;

	UINT64 mid = (m0 >> 32) + (UINT32)m1 + (UINT32)m2;

	UInt128 result;
	result.Low = (m0 & 0xffffffffull) | (mid << 32);
	result.High = m3 + (m1 >> 32) + (m2 >> 32) + (mid >> 32);
	return result;
}

constexpr UInt128 ECC::Add128_128(UInt128 a, UInt128 b)
{
	UInt128 result;
	result.Low = a.Low + b.Low;
	result.High = a.High + b.High + (result.Low < a.Low);

	return result;
}

VOID ECC::VliMult(UINT64 (&result)[ECC_PRODUCT_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS], const UINT64 (&right)[MAX_NUM_ECC_DIGITS])
{
	UInt128 r01 = {0, 0};
	UINT64 r2 = 0;

	UINT32 i, k;

	// Compute each digit of result in sequence, maintaining the carries.
	for (k = 0; k < this->numEccDigits * 2 - 1; ++k)
	{
		UINT32 minIdx = (k < this->numEccDigits ? 0 : (k + 1) - this->numEccDigits);
		for (i = minIdx; i <= k && i < this->numEccDigits; ++i)
		{
			UInt128 product = this->Mul64_64(left[i], right[k - i]);
			r01 = this->Add128_128(r01, product);
			r2 += (r01.High < product.High);
		}
		result[k] = r01.Low;
		r01.Low = r01.High;
		r01.High = r2;
		r2 = 0;
	}

	result[this->numEccDigits * 2 - 1] = r01.Low;
}

VOID ECC::VliSquare(UINT64 (&result)[ECC_PRODUCT_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS])
{
	UInt128 r01 = {0, 0};
	UINT64 r2 = 0;

	UINT32 i, k;
	for (k = 0; k < this->numEccDigits * 2 - 1; ++k)
	{
		UINT32 minIdx = (k < this->numEccDigits ? 0 : (k + 1) - this->numEccDigits);
		for (i = minIdx; i <= k && i <= k - i; ++i)
		{
			UInt128 product = this->Mul64_64(left[i], left[k - i]);
			if (i < k - i)
			{
				r2 += product.High >> 63;
				product.High = (product.High << 1) | (product.Low >> 63);
				product.Low <<= 1;
			}
			r01 = this->Add128_128(r01, product);
			r2 += (r01.High < product.High);
		}
		result[k] = r01.Low;
		r01.Low = r01.High;
		r01.High = r2;
		r2 = 0;
	}

	result[this->numEccDigits * 2 - 1] = r01.Low;
}

/// Computes result = (left + right) % mod.
/// Assumes that left < mod and right < mod, result != mod.
VOID ECC::VliModAdd(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS], const UINT64 (&right)[MAX_NUM_ECC_DIGITS], const UINT64 (&modulus)[MAX_NUM_ECC_DIGITS])
{
	UINT64 carry = this->VliAdd(result, left, right);
	if (carry || this->VliCmp(result, modulus) >= 0)
	{ // result > mod (result = mod + remainder), so subtract mod to get remainder.
		this->VliSub(result, result, modulus);
	}
}

/// Computes result = (left - right) % mod.
/// Assumes that left < mod and right < mod, result != mod.
VOID ECC::VliModSub(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS], const UINT64 (&right)[MAX_NUM_ECC_DIGITS], const UINT64 (&modulus)[MAX_NUM_ECC_DIGITS])
{
	UINT64 borrow = this->VliSub(result, left, right);
	if (borrow)
	{ // In this case, result == -diff == (max int) - diff.
	  // Since -x % d == d - x, we can get the correct result from result + mod (with overflow).
		this->VliAdd(result, result, modulus);
	}
}

/// Computes result = product % curveP
/// from http://www.nsa.gov/ia/_files/nist-routines.pdf
VOID ECC::VliMmodFast256(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&product)[ECC_PRODUCT_DIGITS])
{
	UINT64 tmp[MAX_NUM_ECC_DIGITS];
	INT64 carry;

	// t
	this->VliSet(result, product);

	// s1
	tmp[0] = 0;
	tmp[1] = product[5] & 0xffffffff00000000ull;
	tmp[2] = product[6];
	tmp[3] = product[7];
	carry = this->VliLShift(tmp, tmp, 1);
	carry += this->VliAdd(result, result, tmp);

	// s2
	tmp[1] = product[6] << 32;
	tmp[2] = (product[6] >> 32) | (product[7] << 32);
	tmp[3] = product[7] >> 32;
	carry += this->VliLShift(tmp, tmp, 1);
	carry += this->VliAdd(result, result, tmp);

	// s3
	tmp[0] = product[4];
	tmp[1] = product[5] & 0xffffffff;
	tmp[2] = 0;
	tmp[3] = product[7];
	carry += this->VliAdd(result, result, tmp);
	// s4
	tmp[0] = (product[4] >> 32) | (product[5] << 32);
	tmp[1] = (product[5] >> 32) | (product[6] & 0xffffffff00000000ull);
	tmp[2] = product[7];
	tmp[3] = (product[6] >> 32) | (product[4] << 32);
	carry += this->VliAdd(result, result, tmp);

	// d1
	tmp[0] = (product[5] >> 32) | (product[6] << 32);
	tmp[1] = (product[6] >> 32);
	tmp[2] = 0;
	tmp[3] = (product[4] & 0xffffffff) | (product[5] << 32);
	carry -= this->VliSub(result, result, tmp);

	// d2
	tmp[0] = product[6];
	tmp[1] = product[7];
	tmp[2] = 0;
	tmp[3] = (product[4] >> 32) | (product[5] & 0xffffffff00000000ull);
	carry -= this->VliSub(result, result, tmp);
	// d3
	tmp[0] = (product[6] >> 32) | (product[7] << 32);
	tmp[1] = (product[7] >> 32) | (product[4] << 32);
	tmp[2] = (product[4] >> 32) | (product[5] << 32);
	tmp[3] = (product[6] << 32);
	carry -= this->VliSub(result, result, tmp);
	// d4
	tmp[0] = product[7];
	tmp[1] = product[4] & 0xffffffff00000000ull;
	tmp[2] = product[5];
	tmp[3] = product[6] & 0xffffffff00000000ull;
	carry -= this->VliSub(result, result, tmp);
	if (carry < 0)
	{
		do
		{
			carry += this->VliAdd(result, result, this->curveP);
		} while (carry < 0);
	}
	else
	{
		while (carry || this->VliCmp(this->curveP, result) != 1)
		{
			carry -= this->VliSub(result, result, this->curveP);
		}
	}
}

VOID ECC::OmegaMult384(UINT64 (&result)[ECC_PRODUCT_DIGITS], Span<const UINT64> right)
{
	UINT64 tmp[MAX_NUM_ECC_DIGITS];
	UINT64 carry, diff;

	// Multiply by (2^128 + 2^96 - 2^32 + 1).
	this->VliSet(result, right); // 1
	carry = this->VliLShift(tmp, right, 32);
	result[1 + this->numEccDigits] = carry + this->VliAdd(Span<UINT64>(result + 1, this->numEccDigits), Span<const UINT64>(result + 1, this->numEccDigits), tmp); // 2^96 + 1
	result[2 + this->numEccDigits] = this->VliAdd(Span<UINT64>(result + 2, this->numEccDigits), Span<const UINT64>(result + 2, this->numEccDigits), right);          // 2^128 + 2^96 + 1
	carry += this->VliSub(result, result, tmp);                                          // 2^128 + 2^96 - 2^32 + 1
	diff = result[this->numEccDigits] - carry;
	if (diff > result[this->numEccDigits])
	{ // Propagate borrow if necessary.
		UINT32 i;
		for (i = 1 + this->numEccDigits;; ++i)
		{
			--result[i];
			if (result[i] != (UINT64)-1)
			{
				break;
			}
		}
	}
	result[this->numEccDigits] = diff;
}

/// Computes p_result = p_product % curveP
/// see PDF "Comparing Elliptic Curve Cryptography and RSA on 8-bit CPUs"
/// section "Curve-Specific Optimizations"
VOID ECC::VliMmodFast384(UINT64 (&result)[MAX_NUM_ECC_DIGITS], UINT64 (&product)[ECC_PRODUCT_DIGITS])
{
	UINT64 tmp[2 * MAX_NUM_ECC_DIGITS];

	while (!this->VliIsZero(Span<const UINT64>(product + this->numEccDigits, this->numEccDigits))) // While c1 != 0
	{
		UINT64 carry = 0;
		UINT32 i;

		this->VliClear(tmp);
		this->VliClear(Span<UINT64>(tmp + this->numEccDigits, this->numEccDigits));
		this->OmegaMult384(tmp, Span<const UINT64>(product + this->numEccDigits, this->numEccDigits)); // tmp = w * c1
		this->VliClear(Span<UINT64>(product + this->numEccDigits, this->numEccDigits));            // p = c0

		// (c1, c0) = c0 + w * c1
		for (i = 0; i < this->numEccDigits + 3; ++i)
		{
			UINT64 sum = product[i] + tmp[i] + carry;
			if (sum != product[i])
			{
				carry = (sum < product[i]);
			}
			product[i] = sum;
		}
	}

	while (this->VliCmp(product, this->curveP) > 0)
	{
		this->VliSub(product, product, this->curveP);
	}
	this->VliSet(result, product);
}

/// Dispatches to the curve-specific fast reduction.
VOID ECC::MmodFast(UINT64 (&result)[MAX_NUM_ECC_DIGITS], UINT64 (&product)[ECC_PRODUCT_DIGITS])
{
	if (this->eccBytes == SECP256R1)
		this->VliMmodFast256(result, product);
	else if (this->eccBytes == SECP384R1)
		this->VliMmodFast384(result, product);
}

/// Computes p_result = (p_left * p_right) % curve_p.
VOID ECC::VliModMultFast(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS], const UINT64 (&right)[MAX_NUM_ECC_DIGITS])
{
	UINT64 product[2 * MAX_NUM_ECC_DIGITS];
	this->VliMult(product, left, right);
	this->MmodFast(result, product);
}

/// Computes p_result = p_left^2 % curveP.
VOID ECC::VliModSquareFast(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS])
{
	UINT64 product[2 * MAX_NUM_ECC_DIGITS];
	this->VliSquare(product, left);
	this->MmodFast(result, product);
}

/// Computes p_result = (1 / p_input) % p_mod. All VLIs are the same size.
/// See "From Euclid's GCD to Montgomery Multiplication to the Great Divide"
/// https://labs.oracle.com/techrep/2001/smli_tr-2001-95.pdf
VOID ECC::VliModInv(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&input)[MAX_NUM_ECC_DIGITS], const UINT64 (&modulus)[MAX_NUM_ECC_DIGITS])
{
	UINT64 a[MAX_NUM_ECC_DIGITS], b[MAX_NUM_ECC_DIGITS], u[MAX_NUM_ECC_DIGITS], v[MAX_NUM_ECC_DIGITS];
	UINT64 carry;
	INT32 cmpResult;

	if (this->VliIsZero(input))
	{
		this->VliClear(result);
		return;
	}

	this->VliSet(a, input);
	this->VliSet(b, modulus);
	this->VliClear(u);
	u[0] = 1;
	this->VliClear(v);

	while ((cmpResult = this->VliCmp(a, b)) != 0)
	{
		carry = 0;
		if (IsVliEven(a))
		{
			this->VliRShift1(a);
			if (!IsVliEven(u))
			{
				carry = this->VliAdd(u, u, modulus);
			}
			this->VliRShift1(u);
			if (carry)
			{
				u[this->numEccDigits - 1] |= 0x8000000000000000ull;
			}
		}
		else if (IsVliEven(b))
		{
			this->VliRShift1(b);
			if (!IsVliEven(v))
			{
				carry = this->VliAdd(v, v, modulus);
			}
			this->VliRShift1(v);
			if (carry)
			{
				v[this->numEccDigits - 1] |= 0x8000000000000000ull;
			}
		}
		else if (cmpResult > 0)
		{
			this->VliSub(a, a, b);
			this->VliRShift1(a);
			if (this->VliCmp(u, v) < 0)
			{
				this->VliAdd(u, u, modulus);
			}
			this->VliSub(u, u, v);
			if (!IsVliEven(u))
			{
				carry = this->VliAdd(u, u, modulus);
			}
			this->VliRShift1(u);
			if (carry)
			{
				u[this->numEccDigits - 1] |= 0x8000000000000000ull;
			}
		}
		else
		{
			this->VliSub(b, b, a);
			this->VliRShift1(b);
			if (this->VliCmp(v, u) < 0)
			{
				this->VliAdd(v, v, modulus);
			}
			this->VliSub(v, v, u);
			if (!IsVliEven(v))
			{
				carry = this->VliAdd(v, v, modulus);
			}
			this->VliRShift1(v);
			if (carry)
			{
				v[this->numEccDigits - 1] |= 0x8000000000000000ull;
			}
		}
	}

	this->VliSet(result, u);
}

/// ------ Point operations ------

/// Returns 1 if point is the point at infinity, 0 otherwise.
BOOL ECC::IsZero(const ECCPoint &point)
{
	return (this->VliIsZero(point.X) && this->VliIsZero(point.Y));
}

/// Point multiplication algorithm using Montgomery's ladder with co-Z coordinates.
/// From http://eprint.iacr.org/2011/338.pdf

/// Double in place
VOID ECC::DoubleJacobian(UINT64 (&X1)[MAX_NUM_ECC_DIGITS], UINT64 (&Y1)[MAX_NUM_ECC_DIGITS], UINT64 (&Z1)[MAX_NUM_ECC_DIGITS])
{
	// t1 = X, t2 = Y, t3 = Z
	UINT64 t4[MAX_NUM_ECC_DIGITS];
	UINT64 t5[MAX_NUM_ECC_DIGITS];

	if (this->VliIsZero(Z1))
	{
		return;
	}

	this->VliModSquareFast(t4, Y1);   // t4 = y1^2
	this->VliModMultFast(t5, X1, t4); // t5 = x1*y1^2 = A
	this->VliModSquareFast(t4, t4);   // t4 = y1^4
	this->VliModMultFast(Y1, Y1, Z1); // t2 = y1*z1 = z3
	this->VliModSquareFast(Z1, Z1);   // t3 = z1^2

	this->VliModAdd(X1, X1, Z1, this->curveP); // t1 = x1 + z1^2
	this->VliModAdd(Z1, Z1, Z1, this->curveP); // t3 = 2*z1^2
	this->VliModSub(Z1, X1, Z1, this->curveP); // t3 = x1 - z1^2
	this->VliModMultFast(X1, X1, Z1);          // t1 = x1^2 - z1^4

	this->VliModAdd(Z1, X1, X1, this->curveP); // t3 = 2*(x1^2 - z1^4)
	this->VliModAdd(X1, X1, Z1, this->curveP); // t1 = 3*(x1^2 - z1^4)
	if (this->VliTestBit(X1, 0))
	{
		UINT64 carry = this->VliAdd(X1, X1, this->curveP);
		this->VliRShift1(X1);
		X1[this->numEccDigits - 1] |= carry << 63;
	}
	else
	{
		this->VliRShift1(X1);
	}
	// t1 = 3/2*(x1^2 - z1^4) = B

	this->VliModSquareFast(Z1, X1);            // t3 = B^2
	this->VliModSub(Z1, Z1, t5, this->curveP); // t3 = B^2 - A
	this->VliModSub(Z1, Z1, t5, this->curveP); // t3 = B^2 - 2A = x3
	this->VliModSub(t5, t5, Z1, this->curveP); // t5 = A - x3
	this->VliModMultFast(X1, X1, t5);          // t1 = B * (A - x3)
	this->VliModSub(t4, X1, t4, this->curveP); // t4 = B * (A - x3) - y1^4 = y3
	this->VliSet(X1, Z1);
	this->VliSet(Z1, Y1);
	this->VliSet(Y1, t4);
}

/// Modify (x1, y1) => (x1 * z^2, y1 * z^3)
VOID ECC::ApplyZ(UINT64 (&X1)[MAX_NUM_ECC_DIGITS], UINT64 (&Y1)[MAX_NUM_ECC_DIGITS], UINT64 (&Z)[MAX_NUM_ECC_DIGITS])
{
	UINT64 t1[MAX_NUM_ECC_DIGITS];

	this->VliModSquareFast(t1, Z);    // z^2
	this->VliModMultFast(X1, X1, t1); // x1 * z^2
	this->VliModMultFast(t1, t1, Z);  // z^3
	this->VliModMultFast(Y1, Y1, t1); // y1 * z^3
}

/// P = (x1, y1) => 2P, (x2, y2) => P'
VOID ECC::XYcZInitialDouble(UINT64 (&X1)[MAX_NUM_ECC_DIGITS], UINT64 (&Y1)[MAX_NUM_ECC_DIGITS], UINT64 (&X2)[MAX_NUM_ECC_DIGITS], UINT64 (&Y2)[MAX_NUM_ECC_DIGITS], UINT64 *initialZ)
{
	UINT64 z[MAX_NUM_ECC_DIGITS];

	this->VliSet(X2, X1);
	this->VliSet(Y2, Y1);

	this->VliClear(z);
	z[0] = 1;
	if (initialZ)
	{
		this->VliSet(z, Span<const UINT64>(initialZ, this->numEccDigits));
	}

	this->ApplyZ(X1, Y1, z);

	this->DoubleJacobian(X1, Y1, z);

	this->ApplyZ(X2, Y2, z);
}

/// Input P = (x1, y1, Z), Q = (x2, y2, Z)
/// Output P' = (x1', y1', Z3), P + Q = (x3, y3, Z3)
/// or P => P', Q => P + Q
VOID ECC::XYcZAdd(UINT64 (&X1)[MAX_NUM_ECC_DIGITS], UINT64 (&Y1)[MAX_NUM_ECC_DIGITS], UINT64 (&X2)[MAX_NUM_ECC_DIGITS], UINT64 (&Y2)[MAX_NUM_ECC_DIGITS])
{
	// t1 = X1, t2 = Y1, t3 = X2, t4 = Y2
	UINT64 t5[MAX_NUM_ECC_DIGITS];

	this->VliModSub(t5, X2, X1, this->curveP); // t5 = x2 - x1
	this->VliModSquareFast(t5, t5);            // t5 = (x2 - x1)^2 = A
	this->VliModMultFast(X1, X1, t5);          // t1 = x1*A = B
	this->VliModMultFast(X2, X2, t5);          // t3 = x2*A = C
	this->VliModSub(Y2, Y2, Y1, this->curveP); // t4 = y2 - y1
	this->VliModSquareFast(t5, Y2);            // t5 = (y2 - y1)^2 = D

	this->VliModSub(t5, t5, X1, this->curveP); // t5 = D - B
	this->VliModSub(t5, t5, X2, this->curveP); // t5 = D - B - C = x3
	this->VliModSub(X2, X2, X1, this->curveP); // t3 = C - B
	this->VliModMultFast(Y1, Y1, X2);          // t2 = y1*(C - B)
	this->VliModSub(X2, X1, t5, this->curveP); // t3 = B - x3
	this->VliModMultFast(Y2, Y2, X2);          // t4 = (y2 - y1)*(B - x3)
	this->VliModSub(Y2, Y2, Y1, this->curveP); // t4 = y3
	this->VliSet(X2, t5);
}

/// Input P = (x1, y1, Z), Q = (x2, y2, Z)
/// Output P + Q = (x3, y3, Z3), P - Q = (x3', y3', Z3)
/// or P => P - Q, Q => P + Q
VOID ECC::XYcZAddC(UINT64 (&X1)[MAX_NUM_ECC_DIGITS], UINT64 (&Y1)[MAX_NUM_ECC_DIGITS], UINT64 (&X2)[MAX_NUM_ECC_DIGITS], UINT64 (&Y2)[MAX_NUM_ECC_DIGITS])
{
	// t1 = X1, t2 = Y1, t3 = X2, t4 = Y2
	UINT64 t5[MAX_NUM_ECC_DIGITS];
	UINT64 t6[MAX_NUM_ECC_DIGITS];
	UINT64 t7[MAX_NUM_ECC_DIGITS];

	this->VliModSub(t5, X2, X1, this->curveP); // t5 = x2 - x1
	this->VliModSquareFast(t5, t5);            // t5 = (x2 - x1)^2 = A
	this->VliModMultFast(X1, X1, t5);          // t1 = x1*A = B
	this->VliModMultFast(X2, X2, t5);          // t3 = x2*A = C
	this->VliModAdd(t5, Y2, Y1, this->curveP); // t4 = y2 + y1
	this->VliModSub(Y2, Y2, Y1, this->curveP); // t4 = y2 - y1

	this->VliModSub(t6, X2, X1, this->curveP); // t6 = C - B
	this->VliModMultFast(Y1, Y1, t6);          // t2 = y1 * (C - B)
	this->VliModAdd(t6, X1, X2, this->curveP); // t6 = B + C
	this->VliModSquareFast(X2, Y2);            // t3 = (y2 - y1)^2
	this->VliModSub(X2, X2, t6, this->curveP); // t3 = x3

	this->VliModSub(t7, X1, X2, this->curveP); // t7 = B - x3
	this->VliModMultFast(Y2, Y2, t7);          // t4 = (y2 - y1)*(B - x3)
	this->VliModSub(Y2, Y2, Y1, this->curveP); // t4 = y3

	this->VliModSquareFast(t7, t5);            // t7 = (y2 + y1)^2 = F
	this->VliModSub(t7, t7, t6, this->curveP); // t7 = x3'
	this->VliModSub(t6, t7, X1, this->curveP); // t6 = x3' - B
	this->VliModMultFast(t6, t6, t5);          // t6 = (y2 + y1)*(x3' - B)
	this->VliModSub(Y1, t6, Y1, this->curveP); // t2 = y3'

	this->VliSet(X1, t7);
}

VOID ECC::Mult(ECCPoint &result, ECCPoint &point, UINT64 (&scalar)[MAX_NUM_ECC_DIGITS], UINT64 *initialZ)
{
	// R0 and R1
	UINT64 Rx[2][MAX_NUM_ECC_DIGITS];
	UINT64 Ry[2][MAX_NUM_ECC_DIGITS];
	UINT64 z[MAX_NUM_ECC_DIGITS];

	INT32 i, nb;

	this->VliSet(Rx[1], point.X);
	this->VliSet(Ry[1], point.Y);

	this->XYcZInitialDouble(Rx[1], Ry[1], Rx[0], Ry[0], initialZ);

	// Constant-time: always iterate over all bits to prevent timing leaks
	for (i = this->numEccDigits * 64 - 2; i > 0; --i)
	{
		nb = !(this->VliTestBit(scalar, i));
		this->XYcZAddC(Rx[1 - nb], Ry[1 - nb], Rx[nb], Ry[nb]);
		this->XYcZAdd(Rx[nb], Ry[nb], Rx[1 - nb], Ry[1 - nb]);
	}

	nb = !(this->VliTestBit(scalar, 0));
	this->XYcZAddC(Rx[1 - nb], Ry[1 - nb], Rx[nb], Ry[nb]);
	// Find final 1/Z value.
	this->VliModSub(z, Rx[1], Rx[0], this->curveP); // X1 - X0
	this->VliModMultFast(z, z, Ry[1 - nb]);         // Yb * (X1 - X0)
	this->VliModMultFast(z, z, point.X);            // xP * Yb * (X1 - X0)
	this->VliModInv(z, z, this->curveP);            // 1 / (xP * Yb * (X1 - X0))
	this->VliModMultFast(z, z, point.Y);            // yP / (xP * Yb * (X1 - X0))
	this->VliModMultFast(z, z, Rx[1 - nb]);         // Xb * yP / (xP * Yb * (X1 - X0))
	// End 1/Z calculation

	this->XYcZAdd(Rx[nb], Ry[nb], Rx[1 - nb], Ry[1 - nb]);

	this->ApplyZ(Rx[0], Ry[0], z);

	this->VliSet(result.X, Rx[0]);
	this->VliSet(result.Y, Ry[0]);
}

VOID ECC::Bytes2Native(UINT64 (&native)[MAX_NUM_ECC_DIGITS], Span<const UINT8> bytes)
{
	UINT32 i;
	for (i = 0; i < this->numEccDigits; ++i)
	{
		const UINT8 *digit = bytes.Data() + 8 * (this->numEccDigits - 1 - i);
		native[i] = ((UINT64)digit[0] << 56) | ((UINT64)digit[1] << 48) | ((UINT64)digit[2] << 40) | ((UINT64)digit[3] << 32) |
					 ((UINT64)digit[4] << 24) | ((UINT64)digit[5] << 16) | ((UINT64)digit[6] << 8) | (UINT64)digit[7];
	}
}

VOID ECC::Native2Bytes(Span<UINT8> bytes, const UINT64 (&native)[MAX_NUM_ECC_DIGITS])
{
	UINT32 i;
	for (i = 0; i < this->numEccDigits; ++i)
	{
		UINT8 *digit = bytes.Data() + 8 * (this->numEccDigits - 1 - i);
		digit[0] = (UINT8)(native[i] >> 56);
		digit[1] = (UINT8)(native[i] >> 48);
		digit[2] = (UINT8)(native[i] >> 40);
		digit[3] = (UINT8)(native[i] >> 32);
		digit[4] = (UINT8)(native[i] >> 24);
		digit[5] = (UINT8)(native[i] >> 16);
		digit[6] = (UINT8)(native[i] >> 8);
		digit[7] = (UINT8)native[i];
	}
}

/// Compute a = sqrt(a) (mod curveP).
VOID ECC::ModSqrt(UINT64 (&a)[MAX_NUM_ECC_DIGITS])
{
	UINT32 i;
	UINT64 p1[MAX_NUM_ECC_DIGITS] = {1};
	UINT64 modResult[MAX_NUM_ECC_DIGITS] = {1};

	// Since curveP == 3 (mod 4) for all supported curves, we can
	// compute sqrt(a) = a^((curveP + 1) / 4) (mod curveP).
	this->VliAdd(p1, this->curveP, p1); // p1 = curveP + 1
	for (i = this->VliNumBits(p1) - 1; i > 1; --i)
	{
		this->VliModSquareFast(modResult, modResult);
		if (this->VliTestBit(p1, i))
		{
			this->VliModMultFast(modResult, modResult, a);
		}
	}
	this->VliSet(a, modResult);
}

VOID ECC::PointDecompress(ECCPoint &point, Span<const UINT8> compressed)
{
	UINT64 negA[MAX_NUM_ECC_DIGITS] = {3}; // -a = 3
	this->Bytes2Native(point.X, compressed.Subspan(1));

	this->VliModSquareFast(point.Y, point.X);                      // y = x^2
	this->VliModSub(point.Y, point.Y, negA, this->curveP);          // y = x^2 - 3
	this->VliModMultFast(point.Y, point.Y, point.X);               // y = x^3 - 3x
	this->VliModAdd(point.Y, point.Y, this->curveB, this->curveP); // y = x^3 - 3x + b

	this->ModSqrt(point.Y);

	if ((point.Y[0] & 0x01) != (compressed[0] & 0x01))
	{
		this->VliSub(point.Y, this->curveP, point.Y);
	}
}

ECC::ECC()
{
	Memory::Zero(this, sizeof(ECC));
}

ECC::~ECC()
{
	Memory::Zero(this, sizeof(ECC));
}

Result<UINT32, Error> ECC::ComputeSharedSecret(Span<const UINT8> publicKey, Span<UINT8> secret)
{
	if (publicKey.Size() != this->eccBytes * 2 + 1 || publicKey[0] != 0x04)
		return Result<UINT32, Error>::Err(Error::Ecc_SharedSecretFailed);

	Random random;
	ECCPoint peerPoint;
	UINT64 randomData[MAX_NUM_ECC_DIGITS];

	random.GetArray(Span<UINT8>((UINT8 *)randomData, (USIZE)(this->numEccDigits * sizeof(UINT64))));

	this->Bytes2Native(peerPoint.X, publicKey.Subspan(1, this->eccBytes));
	this->Bytes2Native(peerPoint.Y, publicKey.Subspan(1 + this->eccBytes, this->eccBytes));

	ECCPoint product;
	this->Mult(product, peerPoint, this->privateKey, randomData);
	this->Native2Bytes(Span<UINT8>(secret.Data(), this->eccBytes), product.X);

	if (this->IsZero(product))
		return Result<UINT32, Error>::Err(Error::Ecc_SharedSecretFailed);
	return Result<UINT32, Error>::Ok(this->eccBytes);
}

Result<VOID, Error> ECC::Initialize(INT32 bytes)
{
	this->eccBytes = bytes;
	this->numEccDigits = bytes >> 3;
	if (bytes == SECP256R1)
	{
		const UINT64 pEmbed[] = {0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFF, 0x0000000000000000, 0xFFFFFFFF00000001};
		const UINT64 bEmbed[] = {0x3BCE3C3E27D2604B, 0x651D06B0CC53B0F6, 0xB3EBBD55769886BC, 0x5AC635D8AA3A93E7};
		const UINT64 gxEmbed[] = {0xF4A13945D898C296, 0x77037D812DEB33A0, 0xF8BCE6E563A440F2, 0x6B17D1F2E12C4247};
		const UINT64 gyEmbed[] = {0xCBB6406837BF51F5, 0x2BCE33576B315ECE, 0x8EE7EB4A7C0F9E16, 0x4FE342E2FE1A7F9B};
		const UINT64 nEmbed[] = {0xF3B9CAC2FC632551, 0xBCE6FAADA7179E84, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF00000000};
		Memory::Copy(this->curveP, pEmbed, sizeof(this->curveP));
		Memory::Copy(this->curveB, bEmbed, sizeof(this->curveB));
		Memory::Copy(this->curveG.X, gxEmbed, sizeof(this->curveG.X));
		Memory::Copy(this->curveG.Y, gyEmbed, sizeof(this->curveG.Y));
		Memory::Copy(this->curveN, nEmbed, sizeof(this->curveN));
	}
	else if (bytes == SECP384R1)
	{
		const UINT64 pEmbed[] = {0x00000000FFFFFFFF, 0xFFFFFFFF00000000, 0xFFFFFFFFFFFFFFFE, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF};
		const UINT64 bEmbed[] = {0x2A85C8EDD3EC2AEF, 0xC656398D8A2ED19D, 0x0314088F5013875A, 0x181D9C6EFE814112, 0x988E056BE3F82D19, 0xB3312FA7E23EE7E4};
		const UINT64 gxEmbed[] = {0x3A545E3872760AB7, 0x5502F25DBF55296C, 0x59F741E082542A38, 0x6E1D3B628BA79B98, 0x8EB1C71EF320AD74, 0xAA87CA22BE8B0537};
		const UINT64 gyEmbed[] = {0x7A431D7C90EA0E5F, 0x0A60B1CE1D7E819D, 0xE9DA3113B5F0B8C0, 0xF8F41DBD289A147C, 0x5D9E98BF9292DC29, 0x3617DE4A96262C6F};
		const UINT64 nEmbed[] = {0xECEC196ACCC52973, 0x581A0DB248B0A77A, 0xC7634D81F4372DDF, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF};
		Memory::Copy(this->curveP, pEmbed, sizeof(this->curveP));
		Memory::Copy(this->curveB, bEmbed, sizeof(this->curveB));
		Memory::Copy(this->curveG.X, gxEmbed, sizeof(this->curveG.X));
		Memory::Copy(this->curveG.Y, gyEmbed, sizeof(this->curveG.Y));
		Memory::Copy(this->curveN, nEmbed, sizeof(this->curveN));
	}
	else
		return Result<VOID, Error>::Err(Error::Ecc_InitFailed);

	UINT32 tries = 0;

	do
	{
		Random random;
		random.GetArray(Span<UINT8>((UINT8 *)this->privateKey, (USIZE)(this->numEccDigits * sizeof(UINT64))));
		if (tries++ >= MAX_TRIES)
			return Result<VOID, Error>::Err(Error::Ecc_InitFailed);
		if (this->VliIsZero(this->privateKey))
			continue;

		// Make sure the private key is in the range [1, n-1].
		// For the supported curves, n is always large enough that we only need to subtract once at most.
		if (this->VliCmp(this->curveN, this->privateKey) != 1)
			this->VliSub(this->privateKey, this->privateKey, this->curveN);

		this->Mult(this->publicKey, this->curveG, this->privateKey, nullptr);
	} while (this->IsZero(this->publicKey));
	return Result<VOID, Error>::Ok();
}

Result<UINT32, Error> ECC::ExportPublicKey(Span<UINT8> publicKey)
{
	if (publicKey.Size() < this->eccBytes * 2 + 1)
		return Result<UINT32, Error>::Err(Error::Ecc_ExportKeyFailed);
	publicKey[0] = 0x04;
	this->Native2Bytes(publicKey.Subspan(1, this->eccBytes), this->publicKey.X);
	this->Native2Bytes(publicKey.Subspan(1 + this->eccBytes, this->eccBytes), this->publicKey.Y);
	return Result<UINT32, Error>::Ok(this->eccBytes * 2 + 1);
}