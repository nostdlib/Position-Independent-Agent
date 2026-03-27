#pragma once

#include "core/core.h"
#include "lib/network/tls/tls_buffer.h"
#include "lib/crypto/ecc.h"
#include "lib/network/tls/tls_hash.h"
#include "lib/crypto/chacha20_encoder.h"

#define ECC_COUNT 2
#define RAND_SIZE 32
#define MAX_HASH_LEN 64
#define MAX_PUBKEY_SIZE 2048
#define MAX_KEY_SIZE 32
#define MAX_IV_SIZE 12
#define CIPHER_KEY_SIZE 32
#define CIPHER_HASH_SIZE 32
#define CONTENT_APPLICATION_DATA 0x17

// https://tools.ietf.org/html/rfc4492#section-5.1.1
// https://tools.ietf.org/html/rfc8422#section-5.1.1
// https://tools.ietf.org/html/rfc7919

typedef enum ECC_GROUP
{
	ECC_NONE = 0,           // No ECC Support. It is also used to imply RSA
	ECC_SECP256R1 = 0x0017, // Supported Group: secp256r1(0x0017)
	ECC_SECP384R1 = 0x0018, // Supported Group: secp384r1 (0x0018)
} ECC_GROUP;


// TLS cipher structure
class TlsCipher
{
private:
	INT32 cipherCount;              // Number of supported ciphers
	UINT64 clientSeqNum;            // Client sequence number
	UINT64 serverSeqNum;            // Server sequence number
	ECC *privateEccKeys[ECC_COUNT]; // Private ECC keys
	TlsBuffer publicKey;            // Public key buffer
	TlsBuffer decodeBuffer;         // Buffer for decoded data
	TlsHash handshakeHash;          // Hash for handshake

	union
	{
		struct
		{
			UINT8 mainSecret[MAX_HASH_LEN];      // Main secret
			UINT8 handshakeSecret[MAX_HASH_LEN]; // Handshake secret
			UINT8 pseudoRandomKey[MAX_HASH_LEN]; // Pseudo-random key
		} data13;
		struct
		{
			UINT8 clientRandom[RAND_SIZE]; // Client random value
			UINT8 serverRandom[RAND_SIZE]; // Server random value
			UINT8 masterKey[48];           // Master key
		} data12;
	};
	INT32 cipherIndex;               // Current cipher index
	ChaCha20Encoder chacha20Context; // ChaCha20 encoder context
	BOOL isEncoding;                 // Encoding status

public:
	// Constructor — trivial, call Reset() before use
	TlsCipher() : cipherCount(0), clientSeqNum(0), serverSeqNum(0), privateEccKeys{}, cipherIndex(-1), isEncoding(false) {}
	~TlsCipher() { Destroy(); }

	TlsCipher(const TlsCipher &) = delete;
	TlsCipher &operator=(const TlsCipher &) = delete;

	// Stack-only — placement new/delete required by Result<TlsCipher, Error>
	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}

	TlsCipher(TlsCipher &&other) noexcept
		: cipherCount(other.cipherCount)
		, clientSeqNum(other.clientSeqNum)
		, serverSeqNum(other.serverSeqNum)
		, publicKey(static_cast<TlsBuffer &&>(other.publicKey))
		, decodeBuffer(static_cast<TlsBuffer &&>(other.decodeBuffer))
		, handshakeHash(static_cast<TlsHash &&>(other.handshakeHash))
		, cipherIndex(other.cipherIndex)
		, chacha20Context(static_cast<ChaCha20Encoder &&>(other.chacha20Context))
		, isEncoding(other.isEncoding)
	{
		for (INT32 i = 0; i < ECC_COUNT; i++)
		{
			privateEccKeys[i] = other.privateEccKeys[i];
			other.privateEccKeys[i] = nullptr;
		}
		Memory::Copy(&data13, &other.data13, Math::Max(sizeof(data13), sizeof(data12)));
		// Zero sensitive key material in the moved-from object immediately after copying
		Memory::Zero(&other.data13, Math::Max(sizeof(other.data13), sizeof(other.data12)));
		other.cipherCount = 0;
		other.clientSeqNum = 0;
		other.serverSeqNum = 0;
		other.cipherIndex = -1;
		other.isEncoding = false;
	}

	TlsCipher &operator=(TlsCipher &&other) noexcept
	{
		if (this != &other)
		{
			if (IsValid())
				Destroy();
			cipherCount = other.cipherCount;
			clientSeqNum = other.clientSeqNum;
			serverSeqNum = other.serverSeqNum;
			publicKey = static_cast<TlsBuffer &&>(other.publicKey);
			decodeBuffer = static_cast<TlsBuffer &&>(other.decodeBuffer);
			handshakeHash = static_cast<TlsHash &&>(other.handshakeHash);
			cipherIndex = other.cipherIndex;
			chacha20Context = static_cast<ChaCha20Encoder &&>(other.chacha20Context);
			isEncoding = other.isEncoding;
			for (INT32 i = 0; i < ECC_COUNT; i++)
			{
				privateEccKeys[i] = other.privateEccKeys[i];
				other.privateEccKeys[i] = nullptr;
			}
			Memory::Copy(&data13, &other.data13, Math::Max(sizeof(data13), sizeof(data12)));
			// Zero sensitive key material in the moved-from object immediately after copying
			Memory::Zero(&other.data13, Math::Max(sizeof(other.data13), sizeof(other.data12)));
			other.cipherCount = 0;
			other.clientSeqNum = 0;
			other.serverSeqNum = 0;
			other.cipherIndex = -1;
			other.isEncoding = false;
		}
		return *this;
	}

	// Reset function
	VOID Reset();
	// Destroy function to clean up resources
	VOID Destroy();
	PINT8 CreateClientRand();
	// Function to update server information
	[[nodiscard]] Result<VOID, Error> UpdateServerInfo();
	// Function to get and update the current handshake hash
	VOID GetHash(Span<CHAR> out);
	VOID UpdateHash(Span<const CHAR> in);
	// Key computation functions
	[[nodiscard]] Result<VOID, Error> ComputePublicKey(INT32 eccIndex, TlsBuffer &out);
	[[nodiscard]] Result<VOID, Error> ComputePreKey(ECC_GROUP ecc, Span<const CHAR> serverKey, TlsBuffer &premasterKey);
	[[nodiscard]] Result<VOID, Error> ComputeKey(ECC_GROUP ecc, Span<const CHAR> serverKey, Span<CHAR> finishedHash);
	[[nodiscard]] Result<VOID, Error> ComputeVerify(TlsBuffer &out, INT32 verifySize, INT32 localOrRemote);
	// Functions for encoding and decoding TLS records
	VOID Encode(TlsBuffer &sendbuf, Span<const CHAR> packet, BOOL keepOriginal);
	[[nodiscard]] Result<VOID, Error> Decode(TlsBuffer &inout, INT32 version);
	VOID SetEncoding(BOOL encoding);
	// Function to reset sequence numbers
	VOID ResetSequenceNumber();
	// Check if the cipher is in a valid state
	BOOL IsValid() const { return cipherCount > 0; }
	// Accessor functions
	BOOL GetEncoding() const { return isEncoding; }
	INT32 GetCipherCount() const { return cipherCount; }
	TlsBuffer &GetPubKey() { return publicKey; }
	VOID SetCipherCount(INT32 count) { cipherCount = count; }
};
