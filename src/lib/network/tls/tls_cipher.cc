#include "lib/network/tls/tls_cipher.h"
#include "platform/console/logger.h"
#include "core/memory/memory.h"
#include "platform/system/random.h"
#include "lib/crypto/sha2.h"
#include "lib/network/tls/tls_hkdf.h"
#include "core/math/math.h"

/// @brief Reset the TlsCipher object to its initial state
/// @return void
VOID TlsCipher::Reset()
{
	for (INT32 i = 0; i < ECC_COUNT; i++)
	{
		if (privateEccKeys[i])
		{
			LOG_DEBUG("Freeing ECC key: %p", privateEccKeys[i]);
			delete privateEccKeys[i];
			privateEccKeys[i] = nullptr;
		}
	}
	// Zero out the private ECC key pointers to prevent dangling references
	Memory::Zero(privateEccKeys, sizeof(privateEccKeys));
	publicKey.Clear();
	decodeBuffer.Clear();
	LOG_DEBUG("Resetting tls_cipher structure for cipher: %p", this);
	Memory::Zero(&data12, Math::Max(sizeof(data12), sizeof(data13)));
	SetCipherCount(1);
	clientSeqNum = 0;
	serverSeqNum = 0;
	handshakeHash.Reset();
	cipherIndex = -1;
	isEncoding = false;
}

/// @brief Destroy the TlsCipher object and clean up resources
/// @return void
VOID TlsCipher::Destroy()
{
	Reset();
}

/// @brief Create client random data
/// @return Pointer to the client random data
PINT8 TlsCipher::CreateClientRand()
{
	// Use a local Random instance to generate client random data
	Random random;

	LOG_DEBUG("Creating client random data for cipher: %p", this);
	for (UINT64 i = 0; i < (UINT64)sizeof(data12.clientRandom); i++)
	{
		data12.clientRandom[i] = random.Get() & 0xff;
	}
	LOG_DEBUG("Client random data created: %p", data12.clientRandom);
	return (PINT8)data12.clientRandom;
}

/// @brief Update server information for the TLS cipher
/// @return Result<void, Error>::Ok() if the update was successful
Result<VOID, Error> TlsCipher::UpdateServerInfo()
{
	cipherIndex = 0;

	return Result<VOID, Error>::Ok();
}

/// @brief Get the current handshake hash and store it in the provided output span
/// @param out Output span; size determines which hash algorithm is used
/// @return void
VOID TlsCipher::GetHash(Span<CHAR> out)
{
	handshakeHash.GetHash(out);
}

/// @brief Update the handshake hash with new input data
/// @param in Input span containing the data to be added to the handshake hash
/// @return void
VOID TlsCipher::UpdateHash(Span<const CHAR> in)
{
	handshakeHash.Append(in);
}

/// @brief Compute the public key for the specified ECC index and store it in the provided output buffer
/// @param eccIndex Index of the ECC key to use for public key computation
/// @param out Span to receive the computed public key
/// @return void on success, or an error result if the computation failed
Result<VOID, Error> TlsCipher::ComputePublicKey(INT32 eccIndex, TlsBuffer &out)
{
	if (eccIndex < 0 || eccIndex >= ECC_COUNT)
		return Result<VOID, Error>::Err(Error::TlsCipher_ComputePublicKeyFailed);

	// Allocate ECC key if it doesn't exist
	if (privateEccKeys[eccIndex] == nullptr)
	{
		LOG_DEBUG("Allocating memory for private ECC key at index %d", eccIndex);
		privateEccKeys[eccIndex] = new ECC();
		INT32 ecc_size_list[2];
		ecc_size_list[0] = 32;
		ecc_size_list[1] = 48;
		// Initialize the ECC key and validate initialization
		auto initResult = privateEccKeys[eccIndex]->Initialize(ecc_size_list[eccIndex]);
		if (!initResult)
		{
			LOG_DEBUG("Failed to initialize ECC key at index %d (error: %e)", eccIndex, initResult.Error());
			delete privateEccKeys[eccIndex];
			privateEccKeys[eccIndex] = nullptr;
			return Result<VOID, Error>::Err(initResult, Error::TlsCipher_ComputePublicKeyFailed);
		}
	}
	// Validate size
	auto checkResult = out.CheckSize(MAX_PUBKEY_SIZE);
	if (!checkResult)
		return Result<VOID, Error>::Err(checkResult, Error::TlsCipher_ComputePublicKeyFailed);

	// Export the public key and validate export
	auto exportResult = privateEccKeys[eccIndex]->ExportPublicKey(Span<UINT8>((UINT8 *)out.GetBuffer() + out.GetSize(), MAX_PUBKEY_SIZE));
	if (!exportResult)
		return Result<VOID, Error>::Err(exportResult, Error::TlsCipher_ComputePublicKeyFailed);
	auto setSizeResult = out.SetSize(out.GetSize() + exportResult.Value());
	if (!setSizeResult)
		return Result<VOID, Error>::Err(setSizeResult, Error::TlsCipher_ComputePublicKeyFailed);

	return Result<VOID, Error>::Ok();
}

/// @brief Compute the pre-master key using the specified ECC group and server key, and store it in the provided output buffer
/// @param ecc Specified ECC group to use for key computation
/// @param serverKey Server's public key to use for pre-master key computation
/// @param premasterKey Span to receive the computed pre-master key
/// @return void on success, or an error result if the computation failed
Result<VOID, Error> TlsCipher::ComputePreKey(ECC_GROUP ecc, Span<const CHAR> serverKey, TlsBuffer &premasterKey)
{
	INT32 eccIndex;
	INT32 eccSize;

	// Replace loop with two if statements
	if (ecc == ECC_SECP256R1)
	{
		eccSize = 32;
		eccIndex = 0;
	}
	else if (ecc == ECC_SECP384R1)
	{
		eccSize = 48;
		eccIndex = 1;
	}
	else
	{
		return Result<VOID, Error>::Err(Error::TlsCipher_ComputePreKeyFailed);
	}

	// Compute the public key for the specified ECC group and validate computation
	auto pubKeyResult = ComputePublicKey(eccIndex, publicKey);
	if (!pubKeyResult)
	{
		LOG_DEBUG("Failed to compute public key for ECC group %d (error: %e)", ecc, pubKeyResult.Error());
		return Result<VOID, Error>::Err(pubKeyResult, Error::TlsCipher_ComputePreKeyFailed);
	}
	// Set size 
	auto premasterSizeResult = premasterKey.SetSize(eccSize);
	if (!premasterSizeResult)
		return Result<VOID, Error>::Err(premasterSizeResult, Error::TlsCipher_ComputePreKeyFailed);

	// Compute shared secret using the server's public key and validate computation
	auto secretResult = privateEccKeys[eccIndex]->ComputeSharedSecret(Span<const UINT8>((UINT8 *)serverKey.Data(), serverKey.Size()), Span<UINT8>((UINT8 *)premasterKey.GetBuffer(), eccSize));
	if (!secretResult)
	{
		LOG_DEBUG("Failed to compute shared secret for ECC group %d (error: %e)", ecc, secretResult.Error());
		return Result<VOID, Error>::Err(secretResult, Error::TlsCipher_ComputePreKeyFailed);
	}

	return Result<VOID, Error>::Ok();
}

/// @brief Compute the TLS key using the specified ECC group and server key, and store it in the provided finished hash
/// @param ecc Specified ECC group to use for key computation
/// @param serverKey Server's public key to use for TLS key computation
/// @param finishedHash Span to receive the computed finished hash
/// @return void on success, or an error result if the computation failed
Result<VOID, Error> TlsCipher::ComputeKey(ECC_GROUP ecc, Span<const CHAR> serverKey, Span<CHAR> finishedHash)
{
	if (cipherIndex == -1)
	{
		LOG_DEBUG("Cipher index is -1, cannot compute TLS key");
		return Result<VOID, Error>::Err(Error::TlsCipher_ComputeKeyFailed);
	}
	LOG_DEBUG("Computing TLS key for cipher: %p, ECC group: %d", this, ecc);

	INT32 keyLen = CIPHER_KEY_SIZE;
	INT32 hashLen = CIPHER_HASH_SIZE;

	UINT8 hash[MAX_HASH_LEN];
	UINT8 earlysecret[MAX_HASH_LEN], salt[MAX_HASH_LEN];
	UINT8 localKeyBuffer[MAX_KEY_SIZE], remoteKeyBuffer[MAX_KEY_SIZE];
	UINT8 localIvBuffer[MAX_IV_SIZE], remoteIvBuffer[MAX_IV_SIZE];
	// Declare strings separately to avoid type deduction issues with ternary
	auto server_key_app = "s ap traffic";
	auto server_key_hs = "s hs traffic";
	auto client_key_app = "c ap traffic";
	auto client_key_hs = "c hs traffic";
	const CHAR *server_key = ecc == ECC_NONE ? (const CHAR *)server_key_app : (const CHAR *)server_key_hs;
	const CHAR *client_key = ecc == ECC_NONE ? (const CHAR *)client_key_app : (const CHAR *)client_key_hs;
	TlsHash hash2;
	hash2.GetHash(Span<CHAR>((CHAR *)hash, hashLen));
	Memory::Zero(earlysecret, sizeof(earlysecret));

	// For ECC_NONE, the pre-master key is all zeros and the client/server random values are not used in key computation
	if (ecc == ECC_NONE)
	{
		LOG_DEBUG("Using ECC_NONE for TLS key computation");

		TlsHKDF::ExpandLabel(Span<UCHAR>(salt, hashLen), Span<const UCHAR>((UINT8 *)data13.pseudoRandomKey, hashLen), Span<const CHAR>("derived", 7), Span<const UCHAR>(hash, hashLen));
		TlsHKDF::Extract(Span<UCHAR>(data13.pseudoRandomKey, hashLen), Span<const UCHAR>(salt, hashLen), Span<const UCHAR>(earlysecret, hashLen));

		if (finishedHash.Data())
		{
			LOG_DEBUG("Using finished hash for TLS key computation with size: %d bytes", (INT32)finishedHash.Size());
			Memory::Copy(hash, (VOID *)finishedHash.Data(), hashLen);
		}
	}
	else
	{
		TlsBuffer premaster_key;
		auto preKeyResult = ComputePreKey(ecc, serverKey, premaster_key);
		if (!preKeyResult)
		{
			LOG_DEBUG("Failed to compute pre-master key for ECC group %d", ecc);
			Memory::Zero(hash, sizeof(hash));
			Memory::Zero(earlysecret, sizeof(earlysecret));
			return Result<VOID, Error>::Err(preKeyResult, Error::TlsCipher_ComputeKeyFailed);
		}
		LOG_DEBUG("Computed pre-master key for ECC group %d, size: %d bytes", ecc, premaster_key.GetSize());

		// RFC 8446 §7.1: the initial Extract uses a salt of HashLen zero bytes
		UCHAR zeroSalt[MAX_HASH_LEN];
		Memory::Zero(zeroSalt, hashLen);

		TlsHKDF::Extract(Span<UCHAR>(data13.pseudoRandomKey, hashLen), Span<const UCHAR>(zeroSalt, hashLen), Span<const UCHAR>(earlysecret, hashLen));
		TlsHKDF::ExpandLabel(Span<UCHAR>(salt, hashLen), Span<const UCHAR>(data13.pseudoRandomKey, hashLen), Span<const CHAR>("derived", 7), Span<const UCHAR>(hash, hashLen));
		TlsHKDF::Extract(Span<UCHAR>(data13.pseudoRandomKey, hashLen), Span<const UCHAR>(salt, hashLen), Span<const UCHAR>((UINT8 *)premaster_key.GetBuffer(), premaster_key.GetSize()));

		GetHash(Span<CHAR>((CHAR *)hash, CIPHER_HASH_SIZE));
	}

	TlsHKDF::ExpandLabel(Span<UCHAR>(data13.handshakeSecret, hashLen), Span<const UCHAR>(data13.pseudoRandomKey, hashLen), Span<const CHAR>(client_key, 12), Span<const UCHAR>(hash, hashLen));

	TlsHKDF::ExpandLabel(Span<UCHAR>(localKeyBuffer, keyLen), Span<const UCHAR>(data13.handshakeSecret, hashLen), Span<const CHAR>("key", 3), Span<const UCHAR>());
	TlsHKDF::ExpandLabel(Span<UCHAR>(localIvBuffer, chacha20Context.GetIvLength()), Span<const UCHAR>(data13.handshakeSecret, hashLen), Span<const CHAR>("iv", 2), Span<const UCHAR>());

	TlsHKDF::ExpandLabel(Span<UCHAR>(data13.mainSecret, hashLen), Span<const UCHAR>(data13.pseudoRandomKey, hashLen), Span<const CHAR>(server_key, 12), Span<const UCHAR>(hash, hashLen));

	TlsHKDF::ExpandLabel(Span<UCHAR>(remoteKeyBuffer, keyLen), Span<const UCHAR>(data13.mainSecret, hashLen), Span<const CHAR>("key", 3), Span<const UCHAR>());
	TlsHKDF::ExpandLabel(Span<UCHAR>(remoteIvBuffer, chacha20Context.GetIvLength()), Span<const UCHAR>(data13.mainSecret, hashLen), Span<const CHAR>("iv", 2), Span<const UCHAR>());

	auto initResult = chacha20Context.Initialize(Span<const UINT8, POLY1305_KEYLEN>(localKeyBuffer), Span<const UINT8, POLY1305_KEYLEN>(remoteKeyBuffer), localIvBuffer, remoteIvBuffer);

	Memory::Zero(hash, sizeof(hash));
	Memory::Zero(earlysecret, sizeof(earlysecret));
	Memory::Zero(salt, sizeof(salt));
	Memory::Zero(localKeyBuffer, sizeof(localKeyBuffer));
	Memory::Zero(remoteKeyBuffer, sizeof(remoteKeyBuffer));
	Memory::Zero(localIvBuffer, sizeof(localIvBuffer));
	Memory::Zero(remoteIvBuffer, sizeof(remoteIvBuffer));

	if (!initResult)
	{
		LOG_DEBUG("Failed to initialize encoder (error: %e)", initResult.Error());
		return Result<VOID, Error>::Err(initResult, Error::TlsCipher_ComputeKeyFailed);
	}

	LOG_DEBUG("Encoder initialized successfully");
	return Result<VOID, Error>::Ok();
}

/// @brief Compute the verify data for the TLS handshake and store it in the provided output buffer
/// @param out Pointer to the buffer where the computed verify data will be stored
/// @param verifySize Size of the verify data to compute
/// @param localOrRemote Indicates whether to use the local or remote finished key
/// @return void on success, or an error result if the computation failed
Result<VOID, Error> TlsCipher::ComputeVerify(TlsBuffer &out, INT32 verifySize, INT32 localOrRemote)
{
	if (cipherIndex == -1)
	{
		LOG_DEBUG("tls_cipher_compute_verify: cipher_index is -1, cannot compute verify data");
		return Result<VOID, Error>::Err(Error::TlsCipher_ComputeVerifyFailed);
	}
	CHAR hash[MAX_HASH_LEN];
	INT32 hashLen = CIPHER_HASH_SIZE;
	LOG_DEBUG("tls_cipher_compute_verify: Getting handshake hash, hash_len = %d", hashLen);
	// Get the current handshake hash
	GetHash(Span<CHAR>(hash, hashLen));

	UINT8 finished_key[MAX_HASH_LEN];
	auto finishedLabel = "finished";
	if (localOrRemote)
	{
		LOG_DEBUG("tls_cipher_compute_verify: Using server finished key");
		TlsHKDF::ExpandLabel(Span<UCHAR>(finished_key, hashLen), Span<const UCHAR>(data13.mainSecret, hashLen), Span<const CHAR>(finishedLabel, 8), Span<const UCHAR>());
	}
	else
	{
		LOG_DEBUG("tls_cipher_compute_verify: Using client finished key");
		TlsHKDF::ExpandLabel(Span<UCHAR>(finished_key, hashLen), Span<const UCHAR>(data13.handshakeSecret, hashLen), Span<const CHAR>(finishedLabel, 8), Span<const UCHAR>());
	}
	// Set size and validate 
	auto setSizeResult = out.SetSize(verifySize);
	if (!setSizeResult)
	{
		Memory::Zero(hash, sizeof(hash));
		Memory::Zero(finished_key, sizeof(finished_key));
		return Result<VOID, Error>::Err(setSizeResult, Error::TlsCipher_ComputeVerifyFailed);
	}
	LOG_DEBUG("tls_cipher_compute_verify: Calculating HMAC for verify, verify_size=%d", verifySize);
	HMAC_SHA256 hmac;
	// Initialize HMAC with the finished key and validate initialization and update with the handshake hash
	hmac.Init(Span<const UCHAR>(finished_key, hashLen));
	hmac.Update(Span<const UCHAR>((UINT8 *)hash, hashLen));

	hmac.Final(Span<UCHAR>((UINT8 *)out.GetBuffer(), out.GetSize()));

	Memory::Zero(hash, sizeof(hash));
	Memory::Zero(finished_key, sizeof(finished_key));

	LOG_DEBUG("tls_cipher_compute_verify: Finished verify computation");
	return Result<VOID, Error>::Ok();
}

/// @brief Encode a TLS record using the ChaCha20 encoder and append it to the send buffer
/// @param sendbuf Pointer to the buffer where the encoded TLS record will be appended
/// @param packet TLS record to encode
/// @param keepOriginal Indicates whether to keep the original TLS record without encoding
/// @return void
VOID TlsCipher::Encode(TlsBuffer &sendbuf, Span<const CHAR> packet, BOOL keepOriginal)
{
	if (!isEncoding || keepOriginal)
	{
		LOG_DEBUG("Encoding not enabled or encoder is nullptr, appending packet directly to sendbuf");
		sendbuf.Append(packet);
		return;
	}
	INT32 packetSize = (INT32)packet.Size();
	LOG_DEBUG("Encoding packet with size: %d bytes", packetSize);

	UCHAR aad[13];

	aad[0] = CONTENT_APPLICATION_DATA;
	aad[1] = sendbuf.GetBuffer()[1];
	aad[2] = sendbuf.GetBuffer()[2];
	// Swap 16-bit packet size to big-endian and copy to AAD
	UINT16 encSize = ByteOrder::Swap16(ChaCha20Encoder::ComputeSize(packetSize, CipherDirection::Encode));
	Memory::Copy(aad + 3, &encSize, sizeof(UINT16));
	UINT64 clientSeq = ByteOrder::Swap64(clientSeqNum++);
	Memory::Copy(aad + 5, &clientSeq, sizeof(UINT64));
	// Encode the packet
	chacha20Context.Encode(sendbuf, packet, Span<const UCHAR>(aad));
}

/// @brief Decode a TLS record using the ChaCha20 encoder and store the result in the provided buffer
/// @param inout Pointer to the buffer containing the TLS record to decode, and also where the decoded data will be stored
/// @param version TLS version of the record to decode
/// @return void on success, or an error result if the decoding failed
Result<VOID, Error> TlsCipher::Decode(TlsBuffer &inout, INT32 version)
{
	if (!isEncoding)
	{
		LOG_DEBUG("Encoding not enabled or encoder is nullptr, cannot Decode packet");
		return Result<VOID, Error>::Ok();
	}
	// Initalize AAD with content type and version
	UCHAR aad[13];

	aad[0] = CONTENT_APPLICATION_DATA;
	aad[1] = ByteOrder::Swap16(version) >> 8;
	aad[2] = ByteOrder::Swap16(version) & 0xff;
	UINT16 decSize = ByteOrder::Swap16(inout.GetSize());
	Memory::Copy(aad + 3, &decSize, sizeof(UINT16));
	UINT64 serverSeq = ByteOrder::Swap64(serverSeqNum++);
	Memory::Copy(aad + 5, &serverSeq, sizeof(UINT64));
	// Decode the packet
	auto decodeResult = chacha20Context.Decode(inout, decodeBuffer, Span<const UCHAR>(aad));
	if (!decodeResult)
	{
		LOG_ERROR("Decoding failed, returning error (%e)", decodeResult.Error());
		return Result<VOID, Error>::Err(decodeResult, Error::TlsCipher_DecodeFailed);
	}
	auto setSizeResult = inout.SetSize(decodeBuffer.GetSize());
	if (!setSizeResult)
		return Result<VOID, Error>::Err(setSizeResult, Error::TlsCipher_DecodeFailed);
	Memory::Copy(inout.GetBuffer(), decodeBuffer.GetBuffer(), decodeBuffer.GetSize());
	inout.ResetReadPos();

	return Result<VOID, Error>::Ok();
}

/// @brief Set the encoding status for the TLS cipher
/// @param encoding Indicates whether encoding should be enabled or disabled
/// @return void
VOID TlsCipher::SetEncoding(BOOL encoding)
{
	isEncoding = encoding;
}

/// @brief Request a reset of the sequence numbers for both client and server
/// @return void
VOID TlsCipher::ResetSequenceNumber()
{
	clientSeqNum = 0;
	serverSeqNum = 0;
}
