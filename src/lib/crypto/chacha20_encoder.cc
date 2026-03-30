/**
 * @file chacha20_encoder.cc
 * @brief TLS 1.3 ChaCha20-Poly1305 record layer encoder implementation
 *
 * @details Bidirectional encryption/decryption for TLS 1.3 record layer
 * using ChaCha20-Poly1305 AEAD cipher with per-record nonce derivation.
 */

#include "lib/crypto/chacha20_encoder.h"
#include "platform/console/logger.h"
#include "core/memory/memory.h"

/** @brief TLS record header size in bytes (content type + version + length) */
constexpr INT32 TLS_RECORD_HEADER_SIZE = 5;

ChaCha20Encoder::~ChaCha20Encoder()
{
	Memory::Zero(this->localNonce, TLS_CHACHA20_IV_LENGTH);
	Memory::Zero(this->remoteNonce, TLS_CHACHA20_IV_LENGTH);
	this->initialized = false;
}

ChaCha20Encoder::ChaCha20Encoder()
{
	this->ivLength = TLS_CHACHA20_IV_LENGTH;
	this->initialized = false;
	Memory::Zero(this->localNonce, TLS_CHACHA20_IV_LENGTH);
	Memory::Zero(this->remoteNonce, TLS_CHACHA20_IV_LENGTH);
}

Result<VOID, Error> ChaCha20Encoder::Initialize(Span<const UINT8, POLY1305_KEYLEN> localKey, Span<const UINT8, POLY1305_KEYLEN> remoteKey, const UCHAR (&localIv)[TLS_CHACHA20_IV_LENGTH], const UCHAR (&remoteIv)[TLS_CHACHA20_IV_LENGTH])
{
	UINT32 counter = 1;
	this->ivLength = TLS_CHACHA20_IV_LENGTH;
	LOG_DEBUG("Initializing ChaCha20 encoder with key length: %d bits", (INT32)localKey.Size() * 8);
	auto localSetup = this->localCipher.KeySetup(localKey);
	if (!localSetup)
		return Result<VOID, Error>::Err(localSetup, Error::ChaCha20_KeySetupFailed);
	auto remoteSetup = this->remoteCipher.KeySetup(remoteKey);
	if (!remoteSetup)
		return Result<VOID, Error>::Err(remoteSetup, Error::ChaCha20_KeySetupFailed);
	this->localCipher.IVSetup96BitNonce(localIv, (PUCHAR)&counter);
	Memory::Copy(this->localNonce, localIv, sizeof(localIv));
	this->remoteCipher.IVSetup96BitNonce(remoteIv, (PUCHAR)&counter);
	Memory::Copy(this->remoteNonce, remoteIv, sizeof(remoteIv));

	this->initialized = true;
	return Result<VOID, Error>::Ok();
}

VOID ChaCha20Encoder::Encode(TlsBuffer &out, Span<const CHAR> packet, Span<const UCHAR> aad)
{
	const UCHAR *sequence = aad.Data() + TLS_RECORD_HEADER_SIZE;

	UINT32 counter = 1;
	out.AppendSize((INT32)packet.Size() + POLY1305_TAGLEN);
	UCHAR polyKey[POLY1305_KEYLEN];
	this->localCipher.IVUpdate(this->localNonce, Span<const UINT8, 8>(sequence), (UINT8 *)&counter);
	this->localCipher.Poly1305Key(polyKey);
	this->localCipher.Poly1305Aead(
		Span<const UCHAR>((const UINT8 *)packet.Data(), packet.Size()),
		Span<const UCHAR>(aad.Data(), TLS_RECORD_HEADER_SIZE),
		polyKey,
		Span<UCHAR>((UINT8 *)out.GetBuffer() + out.GetSize() - POLY1305_TAGLEN - (INT32)packet.Size(), (INT32)packet.Size() + POLY1305_TAGLEN));
	Memory::Zero(polyKey, sizeof(polyKey));
}

Result<VOID, Error> ChaCha20Encoder::Decode(TlsBuffer &in, TlsBuffer &out, Span<const UCHAR> aad)
{
	auto checkResult = out.CheckSize(in.GetSize());
	if (!checkResult)
		return Result<VOID, Error>::Err(checkResult, Error::ChaCha20_DecodeFailed);

	const UCHAR *sequence = aad.Data() + TLS_RECORD_HEADER_SIZE;

	UINT32 counter = 1;

	this->remoteCipher.IVUpdate(this->remoteNonce, Span<const UINT8, 8>(sequence), (PUCHAR)&counter);

	UCHAR polyKey[POLY1305_KEYLEN];
	this->remoteCipher.Poly1305Key(polyKey);

	auto decodeResult = this->remoteCipher.Poly1305Decode(
		Span<const UCHAR>((const UINT8 *)in.GetBuffer(), in.GetSize()),
		Span<const UCHAR>(aad.Data(), TLS_RECORD_HEADER_SIZE),
		polyKey,
		Span<UCHAR>((UINT8 *)out.GetBuffer(), in.GetSize()));
	Memory::Zero(polyKey, sizeof(polyKey));
	if (!decodeResult)
	{
		LOG_ERROR("ChaCha20 Decode failed (error: %e)", decodeResult.Error());
		return Result<VOID, Error>::Err(decodeResult, Error::ChaCha20_DecodeFailed);
	} 
	auto& size = decodeResult.Value();
	auto setSizeResult = out.SetSize(size);
	if (!setSizeResult)
		return Result<VOID, Error>::Err(setSizeResult, Error::ChaCha20_DecodeFailed);
	return Result<VOID, Error>::Ok();
}

INT32 ChaCha20Encoder::ComputeSize(INT32 size, CipherDirection direction)
{
	return direction == CipherDirection::Decode ? size - POLY1305_TAGLEN : size + POLY1305_TAGLEN;
}
