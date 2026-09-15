/**
 * @file ecc.h
 * @brief Elliptic Curve Cryptography (ECC) Implementation
 *
 * @details Position-independent implementation of Elliptic Curve Diffie-Hellman (ECDH)
 * key exchange for TLS 1.3. Supports NIST P-256, P-384 curves (secp256r1, secp384r1).
 *
 * Key features:
 * - Pure integer arithmetic (no floating point)
 * - PIC-safe: All curve parameters embedded in code
 * - Constant-time operations where security-critical
 * - Support for compressed and uncompressed point formats
 *
 * Supported curves:
 * - secp256r1 (P-256): 256-bit prime field, 32-byte keys
 * - secp384r1 (P-384): 384-bit prime field, 48-byte keys
 *
 * @par TLS 1.3 Key Exchange:
 * @code
 * ECC ecdh;
 * ecdh.Initialize(32);  // P-256
 *
 * // Export public key for key_share extension
 * UINT8 pubKey[65];  // Uncompressed: 0x04 || x || y
 * ecdh.ExportPublicKey(pubKey, sizeof(pubKey));
 *
 * // Compute shared secret from peer's public key
 * UINT8 secret[32];
 * ecdh.ComputeSharedSecret(peerPubKey, peerPubKeyLen, secret);
 * @endcode
 *
 * @note Uses big integer arithmetic implemented in VLI (Variable Length Integer) operations.
 *
 * @see RFC 8446 Section 4.2.8 — Key Share (TLS 1.3 ECDHE key exchange)
 *      https://datatracker.ietf.org/doc/html/rfc8446#section-4.2.8
 * @see SEC 1 v2 Section 3.2 — Elliptic Curve Key Pair Generation
 *      https://www.secg.org/sec1-v2.pdf
 * @see FIPS 186-4 — Digital Signature Standard (DSS), Appendix D (curve parameters)
 *      https://csrc.nist.gov/publications/detail/fips/186/4/final
 *
 * @ingroup crypt
 *
 * @defgroup ecc Elliptic Curve Cryptography
 * @ingroup crypt
 * @{
 */

#pragma once

#include "core/core.h"

/** @brief Maximum 64-bit words needed for largest supported curve (P-384 = 6 words) */
constexpr INT32 MAX_NUM_ECC_DIGITS = 384 / 64;

/** @brief Double-width product array size for multiplication results */
constexpr INT32 ECC_PRODUCT_DIGITS = 2 * MAX_NUM_ECC_DIGITS;

/**
 * @struct UInt128
 * @brief 128-bit unsigned integer for intermediate multiplication results
 *
 * @details Used internally for 64x64->128 bit multiplication results
 * before reduction. Stored as low/high 64-bit halves.
 */
struct UInt128
{
	UINT64 Low; 
	UINT64 High;
};

/**
 * @struct ECCPoint
 * @brief Elliptic curve point in affine coordinates
 *
 * @details Represents a point (x, y) on the elliptic curve.
 * Coordinates are stored as arrays of 64-bit words in little-endian order.
 * The point at infinity is represented by x = y = 0.
 */
struct ECCPoint
{
	UINT64 X[MAX_NUM_ECC_DIGITS]; /**< @brief X coordinate */
	UINT64 Y[MAX_NUM_ECC_DIGITS]; /**< @brief Y coordinate */
};

/**
 * @class ECC
 * @brief Elliptic Curve Diffie-Hellman (ECDH) Key Exchange
 *
 * @details Implements ECDH key exchange for TLS 1.3 using NIST prime curves.
 * Generates ephemeral key pairs and computes shared secrets for key derivation.
 *
 * The implementation uses:
 * - Jacobian coordinates for point multiplication (faster than affine)
 * - Montgomery ladder for constant-time scalar multiplication
 * - Curve-specific fast reduction for modular arithmetic
 *
 * @par Supported Curves:
 * - secp256r1 (P-256): Initialize with bytes=32
 * - secp384r1 (P-384): Initialize with bytes=48
 *
 * @par Security Considerations:
 * - Private keys are generated randomly and stored internally
 * - Point validation is performed on imported public keys
 * - Shared secret computation is constant-time
 */
class ECC
{
private:
	UINT32 eccBytes;                       /**< @brief Key size in bytes (32 or 48) */
	UINT32 numEccDigits;                   /**< @brief Number of 64-bit words per coordinate */
	UINT64 curveP[MAX_NUM_ECC_DIGITS];     /**< @brief Prime field modulus p */
	UINT64 curveB[MAX_NUM_ECC_DIGITS];     /**< @brief Curve coefficient b (y^2 = x^3 - 3x + b) */
	ECCPoint curveG;                       /**< @brief Base point (generator) G */
	UINT64 curveN[MAX_NUM_ECC_DIGITS];     /**< @brief Order of base point n */
	UINT64 privateKey[MAX_NUM_ECC_DIGITS]; /**< @brief Private key d (random scalar) */
	ECCPoint publicKey;                    /**< @brief Public key Q = d * G */

	// =========================================================================
	// Variable Length Integer (VLI) Operations
	//
	// Parameter style: fixed-size VLI parameters use array references
	// (e.g., UINT64 (&result)[MAX_NUM_ECC_DIGITS]) for compile-time size
	// guarantees. Variable-length sub-arrays (e.g., OmegaMult384's right
	// operand) use Span<const UINT64> since the caller may pass a slice.
	// =========================================================================

	/** @brief Tests if VLI is even (lowest bit is 0) */
	static FORCE_INLINE BOOL IsVliEven(const UINT64 (&vli)[MAX_NUM_ECC_DIGITS]) { return !(vli[0] & 1); }

	/** @brief Sets VLI to zero */
	VOID VliClear(Span<UINT64> vli);

	/** @brief Tests if VLI is zero */
	BOOL VliIsZero(Span<const UINT64> vli);

	/** @brief Tests specific bit of VLI */
	UINT64 VliTestBit(Span<const UINT64> vli, UINT32 bit);

	/** @brief Returns number of significant digits */
	UINT32 VliNumDigits(Span<const UINT64> vli);

	/** @brief Returns number of significant bits */
	UINT32 VliNumBits(Span<const UINT64> vli);

	/** @brief Copies VLI: dest = src */
	VOID VliSet(Span<UINT64> dest, Span<const UINT64> src);

	/** @brief Compares two VLIs: returns -1, 0, or 1 */
	INT32 VliCmp(Span<const UINT64> left, Span<const UINT64> right);

	/** @brief Left shift: result = input << shift */
	UINT64 VliLShift(Span<UINT64> result, Span<const UINT64> input, UINT32 shift);

	/** @brief Right shift by 1: vli >>= 1 */
	VOID VliRShift1(Span<UINT64> vli);

	/** @brief Addition: result = left + right, returns carry */
	UINT64 VliAdd(Span<UINT64> result, Span<const UINT64> left, Span<const UINT64> right);

	/** @brief Subtraction: result = left - right, returns borrow */
	UINT64 VliSub(Span<UINT64> result, Span<const UINT64> left, Span<const UINT64> right);

	/** @brief 64x64 -> 128 bit multiplication */
	constexpr UInt128 Mul64_64(UINT64 left, UINT64 right);

	/** @brief 128-bit addition */
	constexpr UInt128 Add128_128(UInt128 a, UInt128 b);

	/** @brief Full multiplication: result = left * right */
	VOID VliMult(UINT64 (&result)[ECC_PRODUCT_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS], const UINT64 (&right)[MAX_NUM_ECC_DIGITS]);

	/** @brief Squaring: result = left^2 */
	VOID VliSquare(UINT64 (&result)[ECC_PRODUCT_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS]);

	// =========================================================================
	// Modular Arithmetic
	// =========================================================================

	/** @brief Modular addition: result = (left + right) mod modulus */
	VOID VliModAdd(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS], const UINT64 (&right)[MAX_NUM_ECC_DIGITS], const UINT64 (&modulus)[MAX_NUM_ECC_DIGITS]);

	/** @brief Modular subtraction: result = (left - right) mod modulus */
	VOID VliModSub(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS], const UINT64 (&right)[MAX_NUM_ECC_DIGITS], const UINT64 (&modulus)[MAX_NUM_ECC_DIGITS]);

	/** @brief Fast reduction mod p for 256-bit curves (P-256) */
	VOID VliMmodFast256(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&product)[ECC_PRODUCT_DIGITS]);

	/** @brief Helper for P-384 reduction */
	VOID OmegaMult384(UINT64 (&result)[ECC_PRODUCT_DIGITS], Span<const UINT64> right);

	/** @brief Fast reduction mod p for 384-bit curves (P-384) */
	VOID VliMmodFast384(UINT64 (&result)[MAX_NUM_ECC_DIGITS], UINT64 (&product)[ECC_PRODUCT_DIGITS]);

	/** @brief Dispatches to curve-specific fast reduction */
	VOID MmodFast(UINT64 (&result)[MAX_NUM_ECC_DIGITS], UINT64 (&product)[ECC_PRODUCT_DIGITS]);

	/** @brief Fast modular multiplication using curve-specific reduction */
	VOID VliModMultFast(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS], const UINT64 (&right)[MAX_NUM_ECC_DIGITS]);

	/** @brief Fast modular squaring using curve-specific reduction */
	VOID VliModSquareFast(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&left)[MAX_NUM_ECC_DIGITS]);

	/** @brief Modular inverse: result = input^(-1) mod modulus */
	VOID VliModInv(UINT64 (&result)[MAX_NUM_ECC_DIGITS], const UINT64 (&input)[MAX_NUM_ECC_DIGITS], const UINT64 (&modulus)[MAX_NUM_ECC_DIGITS]);

	// =========================================================================
	// Elliptic Curve Point Operations
	// =========================================================================

	/** @brief Tests if point is at infinity */
	BOOL IsZero(const ECCPoint &point);

	/** @brief Point doubling in Jacobian coordinates */
	VOID DoubleJacobian(UINT64 (&X1)[MAX_NUM_ECC_DIGITS], UINT64 (&Y1)[MAX_NUM_ECC_DIGITS], UINT64 (&Z1)[MAX_NUM_ECC_DIGITS]);

	/** @brief Converts from Jacobian to affine coordinates */
	VOID ApplyZ(UINT64 (&X1)[MAX_NUM_ECC_DIGITS], UINT64 (&Y1)[MAX_NUM_ECC_DIGITS], UINT64 (&Z)[MAX_NUM_ECC_DIGITS]);

	/** @brief Initial doubling for co-Z arithmetic */
	VOID XYcZInitialDouble(UINT64 (&X1)[MAX_NUM_ECC_DIGITS], UINT64 (&Y1)[MAX_NUM_ECC_DIGITS], UINT64 (&X2)[MAX_NUM_ECC_DIGITS], UINT64 (&Y2)[MAX_NUM_ECC_DIGITS], UINT64 *initialZ);

	/** @brief Co-Z point addition */
	VOID XYcZAdd(UINT64 (&X1)[MAX_NUM_ECC_DIGITS], UINT64 (&Y1)[MAX_NUM_ECC_DIGITS], UINT64 (&X2)[MAX_NUM_ECC_DIGITS], UINT64 (&Y2)[MAX_NUM_ECC_DIGITS]);

	/** @brief Co-Z conjugate addition */
	VOID XYcZAddC(UINT64 (&X1)[MAX_NUM_ECC_DIGITS], UINT64 (&Y1)[MAX_NUM_ECC_DIGITS], UINT64 (&X2)[MAX_NUM_ECC_DIGITS], UINT64 (&Y2)[MAX_NUM_ECC_DIGITS]);

	/** @brief Scalar multiplication: result = scalar * point */
	VOID Mult(ECCPoint &result, ECCPoint &point, UINT64 (&scalar)[MAX_NUM_ECC_DIGITS], UINT64 *initialZ);

	// =========================================================================
	// Serialization
	// =========================================================================

	/** @brief Converts big-endian bytes to native VLI format */
	VOID Bytes2Native(UINT64 (&native)[MAX_NUM_ECC_DIGITS], Span<const UINT8> bytes);

	/** @brief Converts native VLI format to big-endian bytes */
	VOID Native2Bytes(Span<UINT8> bytes, const UINT64 (&native)[MAX_NUM_ECC_DIGITS]);

	/** @brief Computes modular square root for point decompression */
	VOID ModSqrt(UINT64 (&a)[MAX_NUM_ECC_DIGITS]);

	/** @brief Decompresses point from compressed format (02/03 || x) */
	VOID PointDecompress(ECCPoint &point, Span<const UINT8> compressed);

public:
	/**
	 * @brief Default constructor
	 * @details Initializes internal state. Must call Initialize() before use.
	 */
	ECC();

	/**
	 * @brief Destructor - securely clears private key material
	 */
	~ECC();

	// Non-copyable -- prevent duplication of private key material
	ECC(const ECC &) = delete;
	ECC &operator=(const ECC &) = delete;

	// Non-movable -- ECC objects are heap-allocated and managed through pointers
	ECC(ECC &&) = delete;
	ECC &operator=(ECC &&) = delete;

	/**
	 * @brief Initializes ECC with specified curve
	 * @param bytes Key size in bytes: 32 for P-256, 48 for P-384
	 * @return Ok on success, Err(ECC_InitFailed) on error
	 *
	 * @details Loads curve parameters and generates ephemeral key pair.
	 * The private key is generated randomly; public key is computed as Q = d * G.
	 *
	 * @see SEC 1 v2 Section 3.2 — Elliptic Curve Key Pair Generation
	 *      https://www.secg.org/sec1-v2.pdf
	 * @see FIPS 186-4 Appendix D — Recommended Elliptic Curves
	 *      https://csrc.nist.gov/publications/detail/fips/186/4/final
	 */
	[[nodiscard]] Result<void, Error> Initialize(INT32 bytes);

	/**
	 * @brief Exports public key in uncompressed format
	 * @param publicKey Output span for public key (must be >= 2*eccBytes + 1)
	 * @return Ok(bytesWritten) on success, Err(ECC_ExportKeyFailed) on error
	 *
	 * @details Exports public key as: 0x04 || x || y (uncompressed point format)
	 * For P-256: 65 bytes (1 + 32 + 32)
	 * For P-384: 97 bytes (1 + 48 + 48)
	 *
	 * @see SEC 1 v2 Section 2.3.3 — Elliptic-Curve-Point-to-Octet-String Conversion
	 *      https://www.secg.org/sec1-v2.pdf
	 */
	[[nodiscard]] Result<UINT32, Error> ExportPublicKey(Span<UINT8> publicKey);

	/**
	 * @brief Computes ECDH shared secret
	 * @param publicKey Span of peer's public key (uncompressed format: 0x04 || x || y)
	 * @param secret Output buffer for shared secret (x-coordinate)
	 * @return Ok(bytesWritten) on success, Err(ECC_SharedSecretFailed) on error
	 *
	 * @details Computes shared secret as x-coordinate of d * Q where:
	 * - d is this instance's private key
	 * - Q is the peer's public key
	 *
	 * The public key must be in uncompressed format (first byte 0x04, followed by
	 * x and y coordinates). Compressed point formats are not supported.
	 *
	 * @warning The raw shared secret should be passed through a KDF before use.
	 *
	 * @see RFC 8446 Section 4.2.8.2 — ECDHE Parameters (key_share encoding)
	 *      https://datatracker.ietf.org/doc/html/rfc8446#section-4.2.8.2
	 */
	[[nodiscard]] Result<UINT32, Error> ComputeSharedSecret(Span<const UINT8> publicKey, Span<UINT8> secret);
};

/** @} */ // end of ecc group