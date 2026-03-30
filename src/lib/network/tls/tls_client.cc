#include "lib/network/tls/tls_client.h"
#include "core/binary/binary_reader.h"
#include "core/memory/memory.h"
#include "platform/system/random.h"
#include "platform/socket/socket.h"
#include "core/string/string.h"
#include "platform/platform.h"
#include "platform/console/logger.h"
#include "core/math/math.h"

#define TLS_CHACHA20_POLY1305_SHA256 0x1303

// The following defines SSL 3.0 content types
// CONTENT_APPLICATION_DATA is defined in tls_cipher.h
#define CONTENT_CHANGECIPHERSPEC 0x14
#define CONTENT_ALERT 0x15
#define CONTENT_HANDSHAKE 0x16

// The following defines SSL 3.0/TLS 1.0 Handshake message types
#define MSG_CLIENT_HELLO 0x01
#define MSG_SERVER_HELLO 0x02
#define MSG_ENCRYPTED_EXTENSIONS 0x08 // RFC8446
#define MSG_CERTIFICATE 0x0B
#define MSG_SERVER_HELLO_DONE 0x0E // Not used in TLS1.3
#define MSG_CERTIFICATE_VERIFY 0x0F
#define MSG_CLIENT_KEY_EXCHANGE 0x10 // Not used in TLS1.3
#define MSG_FINISHED 0x14

// This is only used in CONTENT_CHANGECIPHERSPEC content type
#define MSG_CHANGE_CIPHER_SPEC 0x01

// https://www.iana.org/assignments/tls-extensiontype-values/tls-extensiontype-values.xhtml
typedef enum
{
	EXT_SERVER_NAME = 0x0000,          // Type: server_name (0)
	EXT_SUPPORTED_GROUPS = 0x000A,     // Type: supported_groups (10) Y [RFC4492][RFC8422][RFC7748][RFC7919] https://tools.ietf.org/html/rfc8422#section-5.1.1
	EXT_EC_POINT_FORMATS = 0x000B,     // Type: ec_point_formats (11) Y [RFC8422] https://tools.ietf.org/html/rfc8422#section-5.1. https://tools.ietf.org/html/rfc4492#section-5.1.2
	EXT_SIGNATURE_ALGORITHMS = 0x000D, // Type: signature_algorithms (13)

	EXT_ENCRYPT_THEN_MAC = 0x0016,       // Type: encrypt_then_mac(22)
	EXT_EXTENDED_MASTER_SECRET = 0x0017, // Type: extended_master_secret (23)
	EXT_RECORD_SIZE_LIMIT = 0x001C,      // Type: 28	record_size_limit CH, EE Y [RFC8449]
	EXT_SESSIONTICKET_TLS = 0x0023,      // Type: SessionTicket TLS(35)

	EXT_PRESHARED_KEY = 0x0029,          // Type: 41	pre_shared_key CH, SH Y [RFC8446]
	EXT_SUPPORTED_VERSION = 0x002B,      // Type: supported_versions	CH, SH, HRR	Y [RFC8446]
	EXT_PSK_KEY_EXCHANGE_MODES = 0x002D, // Type: psk_key_exchange_modes	CH	Y [RFC8446]
	EXT_KEY_SHARE = 0x0033,              // Type: key_share	CH, SH, HRR	Y [RFC8446]

	EXT_RENEGOTIATION_INFO = 0xFF01, // Type: renegotiation_info(65281)

	EXT_LAST = 0x7FFF
} SSL_EXTENSION;

static FORCE_INLINE VOID AppendU16BE(TlsBuffer &buf, UINT16 val)
{
	buf.Append<INT16>(ByteOrder::Swap16(val));
}

/// @brief Send packet data over TLS connection
/// @param packetType Type of the TLS packet (e.g., handshake, application data)
/// @param ver Version of TLS to use for the packet
/// @param buf The buffer containing the packet data to send
/// @return Result indicating success or Tls_SendPacketFailed error
Result<VOID, Error> TlsClient::SendPacket(INT32 packetType, INT32 ver, TlsBuffer &buf)
{
	if (packetType == CONTENT_HANDSHAKE && buf.GetSize() > 0)
	{
		LOG_DEBUG("Sending handshake packet with type: %d, version: %d, size: %d bytes", packetType, ver, buf.GetSize());
		crypto.UpdateHash(Span<const CHAR>(buf.GetBuffer(), buf.GetSize()));
	}
	LOG_DEBUG("Sending packet with type: %d, version: %d, size: %d bytes", packetType, ver, buf.GetSize());

	// Initialize a temporary buffer to construct the TLS record
	TlsBuffer tempBuffer;
	tempBuffer.Append<CHAR>(packetType);
	tempBuffer.Append<INT16>(ver);
	INT32 bodySizeIndex = tempBuffer.AppendSize(2); // tls body size

	BOOL keepOriginal = packetType == CONTENT_CHANGECIPHERSPEC || packetType == CONTENT_ALERT;
	if (!keepOriginal && crypto.GetEncoding())
	{
		LOG_DEBUG("Encoding packet with type: %d, size: %d bytes", packetType, buf.GetSize());
		buf.Append<CHAR>(packetType);
		(tempBuffer.GetBuffer())[0] = CONTENT_APPLICATION_DATA;
	}
	LOG_DEBUG("Encoding buffer with size: %d bytes, keepOriginal: %d", buf.GetSize(), keepOriginal);
	crypto.Encode(tempBuffer, Span<const CHAR>(buf.GetBuffer(), buf.GetSize()), keepOriginal);
	// Swap the body size to big-endian and write it to the temporary buffer
	UINT16 bodySize = ByteOrder::Swap16(tempBuffer.GetSize() - bodySizeIndex - 2);
	Memory::Copy(tempBuffer.GetBuffer() + bodySizeIndex, &bodySize, sizeof(UINT16));

	// Write it in context and validate it 
	auto writeResult = context.Write(Span<const CHAR>(tempBuffer.GetBuffer(), tempBuffer.GetSize()));
	if (!writeResult)
	{
		LOG_DEBUG("Failed to write packet to socket");
		return Result<VOID, Error>::Err(writeResult, Error::Tls_SendPacketFailed);
	}
	LOG_DEBUG("Packet sent successfully, bytesWritten: %d", writeResult.Value());
	return Result<VOID, Error>::Ok();
}

/// @brief Sent a ClientHello message to initiate the TLS handshake with the server
/// @param host The hostname of the server to connect to
/// @return Result indicating success or Tls_ClientHelloFailed error
Result<VOID, Error> TlsClient::SendClientHello(const CHAR *host)
{
	LOG_DEBUG("Sending ClientHello for client: %p, host: %s", this, host);

	sendBuffer.Clear();

	BOOL hastls13 = false;

	// Construct the ClientHello message according to TLS specifications
	sendBuffer.Append<CHAR>(MSG_CLIENT_HELLO);
	INT32 handshakeSizeIndex = sendBuffer.AppendSize(3); // tls handshake body size
	LOG_DEBUG("Appending ClientHello with handshake size index: %d", handshakeSizeIndex);

	sendBuffer.Append<INT16>(0x0303);
	LOG_DEBUG("Appending ClientHello with version: 0x0303");
	sendBuffer.Append(Span<const CHAR>((const CHAR *)crypto.CreateClientRand(), RAND_SIZE));
	LOG_DEBUG("Appending ClientHello with client random data");
	sendBuffer.Append<CHAR>(0);
	LOG_DEBUG("Client has %d ciphers to append", crypto.GetCipherCount());
	INT32 cipherCountIndex = sendBuffer.AppendSize(2);
	LOG_DEBUG("Appending ClientHello with cipher count index: %d", cipherCountIndex);
	for (INT32 i = 0; i < crypto.GetCipherCount(); i++)
	{
		AppendU16BE(sendBuffer, (UINT16)TLS_CHACHA20_POLY1305_SHA256);
		hastls13 = true;
	}
	LOG_DEBUG("Appending ClientHello with %d ciphers", crypto.GetCipherCount());
	UINT16 cipherSize = ByteOrder::Swap16(sendBuffer.GetSize() - cipherCountIndex - 2);
	Memory::Copy(sendBuffer.GetBuffer() + cipherCountIndex, &cipherSize, sizeof(UINT16));
	sendBuffer.Append<CHAR>(1);
	sendBuffer.Append<CHAR>(0);

	INT32 extSizeIndex = sendBuffer.AppendSize(2);
	LOG_DEBUG("Appending ClientHello with extension size index: %d", extSizeIndex);
	AppendU16BE(sendBuffer, EXT_SERVER_NAME);
	INT32 hostLen = (INT32)StringUtils::Length((PCHAR)host);
	LOG_DEBUG("Appending ClientHello with host: %s, length: %d", host, hostLen);
	AppendU16BE(sendBuffer, hostLen + 5);
	AppendU16BE(sendBuffer, hostLen + 3);
	sendBuffer.Append<CHAR>(0);
	AppendU16BE(sendBuffer, hostLen);
	sendBuffer.Append(Span<const CHAR>(host, hostLen));

	AppendU16BE(sendBuffer, EXT_SUPPORTED_GROUPS); // ext type
	AppendU16BE(sendBuffer, ECC_COUNT * 2 + 2);    // ext size
	AppendU16BE(sendBuffer, ECC_COUNT * 2);
	LOG_DEBUG("Appending ClientHello with supported groups, count: %d", ECC_COUNT);
	AppendU16BE(sendBuffer, (UINT16)ECC_SECP256R1);
	AppendU16BE(sendBuffer, (UINT16)ECC_SECP384R1);

	if (hastls13)
	{
		LOG_DEBUG("Appending ClientHello with TLS 1.3 specific extensions");
		AppendU16BE(sendBuffer, EXT_SUPPORTED_VERSION);
		AppendU16BE(sendBuffer, 3);
		sendBuffer.Append<CHAR>(2);
		// tls 1.3 version
		AppendU16BE(sendBuffer, 0x0304);

		AppendU16BE(sendBuffer, EXT_SIGNATURE_ALGORITHMS);
		AppendU16BE(sendBuffer, 24);
		AppendU16BE(sendBuffer, 22);
		AppendU16BE(sendBuffer, 0x0403);
		AppendU16BE(sendBuffer, 0x0503);
		AppendU16BE(sendBuffer, 0x0603);
		AppendU16BE(sendBuffer, 0x0804);
		AppendU16BE(sendBuffer, 0x0805);
		AppendU16BE(sendBuffer, 0x0806);
		AppendU16BE(sendBuffer, 0x0401);
		AppendU16BE(sendBuffer, 0x0501);
		AppendU16BE(sendBuffer, 0x0601);
		AppendU16BE(sendBuffer, 0x0203);
		AppendU16BE(sendBuffer, 0x0201);

		AppendU16BE(sendBuffer, EXT_KEY_SHARE); // ext type
		INT32 shareSize = sendBuffer.AppendSize(2);
		sendBuffer.AppendSize(2);
		UINT16 ecc_iana_list[2]{};
		ecc_iana_list[0] = ECC_SECP256R1;
		ecc_iana_list[1] = ECC_SECP384R1;

		for (INT32 i = 0; i < ECC_COUNT; i++)
		{
			UINT16 eccIana = ecc_iana_list[i];
			AppendU16BE(sendBuffer, eccIana);
			INT32 shareSizeSub = sendBuffer.AppendSize(2);
			auto r = crypto.ComputePublicKey(i, sendBuffer);
			if (!r)
			{
				LOG_DEBUG("Failed to compute public key for ECC group %d", i);
				return Result<VOID, Error>::Err(r, Error::Tls_ClientHelloFailed);
			}
			LOG_DEBUG("Computed public key for ECC group %d, size: %d bytes", i, sendBuffer.GetSize() - shareSizeSub - 2);
			UINT16 shareSizeVal = ByteOrder::Swap16(sendBuffer.GetSize() - shareSizeSub - 2);
			Memory::Copy(sendBuffer.GetBuffer() + shareSizeSub, &shareSizeVal, sizeof(UINT16));
		}
		UINT16 shareTotal = ByteOrder::Swap16(sendBuffer.GetSize() - shareSize - 2);
		Memory::Copy(sendBuffer.GetBuffer() + shareSize, &shareTotal, sizeof(UINT16));
		UINT16 shareInner = ByteOrder::Swap16(sendBuffer.GetSize() - shareSize - 4);
		Memory::Copy(sendBuffer.GetBuffer() + shareSize + 2, &shareInner, sizeof(UINT16));
	}
	LOG_DEBUG("Appending ClientHello with extensions, size: %d bytes", sendBuffer.GetSize() - extSizeIndex - 2);

	UINT16 extSizeVal = ByteOrder::Swap16(sendBuffer.GetSize() - extSizeIndex - 2);
	Memory::Copy(sendBuffer.GetBuffer() + extSizeIndex, &extSizeVal, sizeof(UINT16));
	sendBuffer.GetBuffer()[handshakeSizeIndex] = 0;
	UINT16 handshakeSize = ByteOrder::Swap16(sendBuffer.GetSize() - handshakeSizeIndex - 3);
	Memory::Copy(sendBuffer.GetBuffer() + handshakeSizeIndex + 1, &handshakeSize, sizeof(UINT16));

	auto r = SendPacket(CONTENT_HANDSHAKE, 0x303, sendBuffer);
	if (!r)
		return Result<VOID, Error>::Err(r, Error::Tls_ClientHelloFailed);
	return Result<VOID, Error>::Ok();
}

/// @brief Send a Client Finished message to complete the TLS handshake
/// @return Result indicating success or Tls_ClientFinishedFailed error
Result<VOID, Error> TlsClient::SendClientFinished()
{
	TlsBuffer verify;
	sendBuffer.Clear();
	LOG_DEBUG("Sending Client Finished for client: %p", this);
	auto verifyResult = crypto.ComputeVerify(verify, CIPHER_HASH_SIZE, 0);
	if (!verifyResult)
		return Result<VOID, Error>::Err(verifyResult, Error::Tls_ClientFinishedFailed);

	LOG_DEBUG("Computed verify data for Client Finished, size: %d bytes", verify.GetSize());
	sendBuffer.Append<CHAR>(MSG_FINISHED);
	sendBuffer.Append<CHAR>(0);
	sendBuffer.Append<INT16>(ByteOrder::Swap16(verify.GetSize()));
	sendBuffer.Append(Span<const CHAR>(verify.GetBuffer(), verify.GetSize()));

	auto r = SendPacket(CONTENT_HANDSHAKE, 0x303, sendBuffer);
	if (!r)
		return Result<VOID, Error>::Err(r, Error::Tls_ClientFinishedFailed);
	return Result<VOID, Error>::Ok();
}

/// @brief Send a Client Key Exchange message to the server during the TLS handshake
/// @return Result indicating success or Tls_ClientExchangeFailed error
Result<VOID, Error> TlsClient::SendClientExchange()
{
	sendBuffer.Clear();
	TlsBuffer &pubkey = crypto.GetPubKey();
	LOG_DEBUG("Sending Client Key Exchange for client: %p, public key size: %d bytes", this, pubkey.GetSize());
	sendBuffer.Append<CHAR>(MSG_CLIENT_KEY_EXCHANGE);
	sendBuffer.Append<CHAR>(0);
	sendBuffer.Append<INT16>(ByteOrder::Swap16(pubkey.GetSize() + 1));
	sendBuffer.Append<CHAR>((pubkey.GetSize())); // tls body size
	sendBuffer.Append(Span<const CHAR>(pubkey.GetBuffer(), pubkey.GetSize()));
	auto r = SendPacket(CONTENT_HANDSHAKE, 0x303, sendBuffer);
	if (!r) 
		return Result<VOID, Error>::Err(r, Error::Tls_ClientExchangeFailed);
	return Result<VOID, Error>::Ok();
}

/// @brief Send a Change Cipher Spec message to the server to indicate that subsequent messages will be encrypted
/// @return Result indicating success or Tls_ChangeCipherSpecFailed error
Result<VOID, Error> TlsClient::SendChangeCipherSpec()
{
	sendBuffer.Clear();
	sendBuffer.Append<CHAR>(1);
	auto r = SendPacket(CONTENT_CHANGECIPHERSPEC, 0x303, sendBuffer);
	if (!r)
		return Result<VOID, Error>::Err(r, Error::Tls_ChangeCipherSpecFailed);
	return Result<VOID, Error>::Ok();
}

/// @brief Process the ServerHello message from the server and advances the TLS handshake state
/// @param reader Buffer containing the ServerHello message data
/// @return Result indicating success or Tls_ServerHelloFailed error
Result<VOID, Error> TlsClient::OnServerHello(TlsBuffer &reader)
{
	CHAR serverRand[RAND_SIZE];

	LOG_DEBUG("Processing ServerHello for client: %p", this);
	reader.ReadU24BE();     // handshake body size (already bounded by TLS record)
	reader.Read<INT16>();   // version (skip)
	reader.Read(Span<CHAR>(serverRand, sizeof(serverRand)));
	INT32 sessionLen = reader.Read<INT8>();
	LOG_DEBUG("ServerHello session length: %d", sessionLen);
	reader.AdvanceReadPosition(sessionLen);
	reader.Read<INT16>(); // cur_cipher
	reader.Read<INT8>();
	auto ret = crypto.UpdateServerInfo();
	if (!ret)
	{
		LOG_DEBUG("Failed to update server info for client: %p", this);
		return Result<VOID, Error>::Err(ret, Error::Tls_ServerHelloFailed);
	}

	if (reader.GetReadPosition() >= reader.GetSize())
	{
		LOG_DEBUG("ServerHello reader has reached the end of buffer, no extensions found");
		return Result<VOID, Error>::Ok();
	}
	LOG_DEBUG("ServerHello has extensions, processing them");

	INT32 extSize = ByteOrder::Swap16(reader.Read<INT16>());
	INT32 extStart = reader.GetReadPosition();
	INT32 tlsVer = 0;
	LOG_DEBUG("ServerHello extensions size: %d bytes, start index: %d", extSize, extStart);
	TlsBuffer pubkey;
	ECC_GROUP eccgroup = ECC_NONE;
	while (reader.GetReadPosition() < extStart + extSize)
	{
		SSL_EXTENSION type = (SSL_EXTENSION)ByteOrder::Swap16(reader.Read<INT16>());
		if (type == EXT_SUPPORTED_VERSION)
		{
			LOG_DEBUG("Processing EXT_SUPPORTED_VERSION extension");
			reader.Read<INT16>();
			tlsVer = ByteOrder::Swap16(reader.Read<INT16>());
		}
		else if (type == EXT_KEY_SHARE)
		{
			LOG_DEBUG("Processing EXT_KEY_SHARE extension");
			INT32 size = ByteOrder::Swap16(reader.Read<INT16>());
			eccgroup = (ECC_GROUP)ByteOrder::Swap16(reader.Read<INT16>());
			if (size > 4)
			{
				LOG_DEBUG("Reading public key from EXT_KEY_SHARE, size: %d bytes", size);
				(VOID)pubkey.SetSize(ByteOrder::Swap16(reader.Read<INT16>()));
				reader.Read(Span<CHAR>(pubkey.GetBuffer(), pubkey.GetSize()));
			}
			LOG_DEBUG("EXT_KEY_SHARE processed, ECC group: %d, public key size: %d bytes", eccgroup, pubkey.GetSize());
		}
		else
		{
			// Skip unknown extensions
			INT32 extLen = ByteOrder::Swap16(reader.Read<INT16>());
			reader.AdvanceReadPosition(extLen);
		}
	}
	if (tlsVer != 0)
	{
		LOG_DEBUG("TLS version from ServerHello: %d", tlsVer);
		if (tlsVer != 0x0304 || pubkey.GetSize() <= 0 || eccgroup == ECC_NONE)
		{
			LOG_DEBUG("Invalid TLS version or public key size, tlsVer: %d, pubkey.size: %d, eccgroup: %d", tlsVer, pubkey.GetSize(), eccgroup);
			return Result<VOID, Error>::Err(Error::Tls_ServerHelloFailed);
		}

		LOG_DEBUG("Valid TLS version and public key size, tlsVer: %d, pubkey.size: %d, eccgroup: %d", tlsVer, pubkey.GetSize(), eccgroup);

		auto r = crypto.ComputeKey(eccgroup, Span<const CHAR>(pubkey.GetBuffer(), pubkey.GetSize()), Span<CHAR>());
		if (!r)
		{
			LOG_DEBUG("Failed to compute TLS 1.3 key for client: %p, ECC group: %d, public key size: %d", this, eccgroup, pubkey.GetSize());
			return Result<VOID, Error>::Err(r, Error::Tls_ServerHelloFailed);
		}
		LOG_DEBUG("Computed TLS 1.3 key for client: %p, ECC group: %d, public key size: %d", this, eccgroup, pubkey.GetSize());
		crypto.SetEncoding(true);
	}
	LOG_DEBUG("ServerHello processed successfully for client: %p, ECC group: %d, public key size: %d", this, eccgroup, pubkey.GetSize());
	return Result<VOID, Error>::Ok();
}

/// @brief Process the ServerHelloDone message from the server and advances the TLS handshake state
/// @return Result indicating success or Tls_ServerHelloDoneFailed error
Result<VOID, Error> TlsClient::OnServerHelloDone()
{
	auto r = SendClientExchange();
	if (!r)
	{
		LOG_DEBUG("Failed to send Client Key Exchange for client: %p", this);
		return Result<VOID, Error>::Err(r, Error::Tls_ServerHelloDoneFailed);
	}
	LOG_DEBUG("Client Key Exchange sent successfully for client: %p", this);
	r = SendChangeCipherSpec();
	if (!r)
	{
		LOG_DEBUG("Failed to send Change Cipher Spec for client: %p", this);
		return Result<VOID, Error>::Err(r, Error::Tls_ServerHelloDoneFailed);
	}
	LOG_DEBUG("Change Cipher Spec sent successfully for client: %p", this);
	crypto.SetEncoding(true);
	r = SendClientFinished();
	if (!r)
	{
		LOG_DEBUG("Failed to send Client Finished for client: %p", this);
		return Result<VOID, Error>::Err(r, Error::Tls_ServerHelloDoneFailed);
	}
	LOG_DEBUG("Client Finished sent successfully for client: %p", this);

	return Result<VOID, Error>::Ok();
}

/// @brief Verify the Finished message from the server by comparing the verify data with the expected value computed from the handshake messages
/// @param reader Buffer containing the Finished message data from the server
/// @return Result indicating success or Tls_VerifyFinishedFailed error
Result<VOID, Error> TlsClient::VerifyFinished(TlsBuffer &reader)
{
	INT32 server_finished_size = reader.ReadU24BE();
	if (server_finished_size < 0 || server_finished_size > reader.GetSize() - reader.GetReadPosition())
		return Result<VOID, Error>::Err(Error::Tls_VerifyFinishedFailed);
	LOG_DEBUG("Verifying Finished for client: %p, size: %d bytes", this, server_finished_size);
	TlsBuffer verify;
	auto verifyResult = crypto.ComputeVerify(verify, server_finished_size, 1);
	if (!verifyResult)
		return Result<VOID, Error>::Err(verifyResult, Error::Tls_VerifyFinishedFailed);
	LOG_DEBUG("Computed verify data for Finished, size: %d bytes", verify.GetSize());

	if (Memory::Compare(verify.GetBuffer(), reader.GetBuffer() + reader.GetReadPosition(), server_finished_size) != 0)
	{
		LOG_DEBUG("Finished verification failed for client: %p, expected size: %d, actual size: %d", this, verify.GetSize(), server_finished_size);
		return Result<VOID, Error>::Err(Error::Tls_VerifyFinishedFailed);
	}
	LOG_DEBUG("Finished verification succeeded for client: %p", this);
	return Result<VOID, Error>::Ok();
}

/// @brief Finished message from the server has been received, process it and advance the TLS handshake state to complete the handshake
/// @return Result indicating success or Tls_ServerFinishedFailed error
Result<VOID, Error> TlsClient::OnServerFinished()
{
	LOG_DEBUG("Processing Server Finished for client: %p", this);
	CHAR finished_hash[MAX_HASH_LEN] = {0};
	crypto.GetHash(Span<CHAR>(finished_hash, CIPHER_HASH_SIZE));
	auto ret = SendChangeCipherSpec();

	if (!ret)
	{
		LOG_DEBUG("Failed to send Change Cipher Spec for client: %p", this);
		return Result<VOID, Error>::Err(ret, Error::Tls_ServerFinishedFailed);
	}
	LOG_DEBUG("Change Cipher Spec sent successfully for client: %p", this);
	auto r = SendClientFinished();
	if (!r)
	{
		LOG_DEBUG("Failed to send Client Finished for client: %p", this);
		return Result<VOID, Error>::Err(r, Error::Tls_ServerFinishedFailed);
	}
	LOG_DEBUG("Client Finished sent successfully for client: %p", this);
	crypto.ResetSequenceNumber();
	auto r2 = crypto.ComputeKey(ECC_NONE, Span<const CHAR>(), Span<CHAR>(finished_hash, CIPHER_HASH_SIZE));
	if (!r2)
	{
		LOG_DEBUG("Failed to compute TLS 1.3 key for client: %p", this);
		return Result<VOID, Error>::Err(r2, Error::Tls_ServerFinishedFailed);
	}

	LOG_DEBUG("Server Finished processed successfully for client: %p", this);
	return Result<VOID, Error>::Ok();
}

/// @brief Process incoming TLS packets from the server, handle different packet types and advance the TLS handshake state accordingly
/// @param packetType Type of the incoming TLS packet (e.g., handshake, alert)
/// @param version Version of TLS used in the packet
/// @param TlsReader Buffer containing the packet data to process
/// @return Result indicating success or Tls_OnPacketFailed error
Result<VOID, Error> TlsClient::OnPacket(INT32 packetType, INT32 version, TlsBuffer &TlsReader)
{
	if (packetType != CONTENT_CHANGECIPHERSPEC && packetType != CONTENT_ALERT)
	{
		LOG_DEBUG("Processing packet with type: %d, version: %d, size: %d bytes", packetType, version, TlsReader.GetSize());
		auto r = crypto.Decode(TlsReader, version);
		if (!r)
		{
			LOG_DEBUG("Failed to Decode packet for client: %p, type: %d, version: %d", this, packetType, version);
			return Result<VOID, Error>::Err(r, Error::Tls_OnPacketFailed);
		}
		LOG_DEBUG("Packet decoded successfully for client: %p, type: %d, version: %d", this, packetType, version);
		if (crypto.GetEncoding() && TlsReader.GetSize() > 0)
		{
			LOG_DEBUG("Removing last byte from buffer for client: %p, packet type: %d", this, packetType);
			packetType = TlsReader.GetBuffer()[TlsReader.GetSize() - 1];
			(VOID)TlsReader.SetSize(TlsReader.GetSize() - 1);
		}
		LOG_DEBUG("Packet type after processing: %d, buffer size: %d bytes", packetType, TlsReader.GetSize());
	}

	TlsState state_seq[6]{};
	state_seq[0] = {CONTENT_HANDSHAKE, MSG_SERVER_HELLO};
	state_seq[1] = {CONTENT_CHANGECIPHERSPEC, MSG_CHANGE_CIPHER_SPEC};
	state_seq[2] = {CONTENT_HANDSHAKE, MSG_ENCRYPTED_EXTENSIONS};
	state_seq[3] = {CONTENT_HANDSHAKE, MSG_CERTIFICATE};
	state_seq[4] = {CONTENT_HANDSHAKE, MSG_CERTIFICATE_VERIFY};
	state_seq[5] = {CONTENT_HANDSHAKE, MSG_FINISHED};

	while (TlsReader.GetReadPosition() < TlsReader.GetSize())
	{
		INT32 seg_size;
		if (packetType == CONTENT_HANDSHAKE)
		{
			INT32 remaining = TlsReader.GetSize() - TlsReader.GetReadPosition();
			if (remaining < 4)
				return Result<VOID, Error>::Err(Error::Tls_OnPacketFailed);
			PUCHAR seg = (PUCHAR)(TlsReader.GetBuffer() + TlsReader.GetReadPosition());
			seg_size = 4 + (((UINT32)seg[1] << 16) | ((UINT32)seg[2] << 8) | (UINT32)seg[3]);
			if (seg_size > remaining)
				return Result<VOID, Error>::Err(Error::Tls_OnPacketFailed);
		}
		else
		{
			seg_size = TlsReader.GetSize();
		}
		TlsBuffer reader_sig(Span<CHAR>(TlsReader.GetBuffer() + TlsReader.GetReadPosition(), (USIZE)seg_size));

		if (stateIndex < 6 && packetType != CONTENT_ALERT)
		{
			LOG_DEBUG("Checking state sequence for client: %p, state index: %d, packet type: %d, handshake type: %d", this, stateIndex, packetType, reader_sig.GetBuffer()[0]);
			if (state_seq[stateIndex].contentType != packetType || state_seq[stateIndex].handshakeType != reader_sig.GetBuffer()[0])
			{
				LOG_DEBUG("State sequence mismatch for client: %p, expected type: %d, expected handshake type: %d, actual type: %d, actual handshake type: %d",
						  this, state_seq[stateIndex].contentType, state_seq[stateIndex].handshakeType, packetType, reader_sig.GetBuffer()[0]);
				return Result<VOID, Error>::Err(Error::Tls_OnPacketFailed);
			}
			LOG_DEBUG("State sequence matches for client: %p, state index: %d, packet type: %d, handshake type: %d", this, stateIndex, packetType, reader_sig.GetBuffer()[0]);
			stateIndex++;
		}

		if (packetType == CONTENT_HANDSHAKE && reader_sig.GetSize() > 0 && reader_sig.GetBuffer()[0] != MSG_FINISHED)
		{
			LOG_DEBUG("Updating hash for client: %p, packet type: %d, size: %d bytes", this, packetType, reader_sig.GetSize());
			crypto.UpdateHash(Span<const CHAR>(reader_sig.GetBuffer(), reader_sig.GetSize()));
		}
		if (packetType == CONTENT_HANDSHAKE)
		{
			LOG_DEBUG("Processing handshake packet for client: %p, handshake type: %d", this, reader_sig.GetBuffer()[0]);
			INT32 handshakeType = reader_sig.Read<INT8>();
			LOG_DEBUG("Handshake type: %d", handshakeType);
			if (handshakeType == MSG_SERVER_HELLO)
			{
				LOG_DEBUG("Processing ServerHello for client: %p", this);
				auto r = OnServerHello(reader_sig);
				if (!r)
				{
					LOG_DEBUG("Failed to process handshake packet for client: %p, handshake type: %d", this, handshakeType);
					(VOID)Close();
					return Result<VOID, Error>::Err(r, Error::Tls_OnPacketFailed);
				}
			}

			else if (handshakeType == MSG_CERTIFICATE)
			{
				LOG_DEBUG("Processing Server Certificate for client: %p", this);
			}

			else if (handshakeType == MSG_CERTIFICATE_VERIFY)
			{
				LOG_DEBUG("Processing Server Certificate Verify for client: %p", this);
			}
			else if (handshakeType == MSG_SERVER_HELLO_DONE)
			{
				LOG_DEBUG("Processing Server Hello Done for client: %p", this);
				auto r = OnServerHelloDone();
				if (!r)
				{
					LOG_DEBUG("Failed to process Server Hello Done for client: %p", this);
					return Result<VOID, Error>::Err(r, Error::Tls_OnPacketFailed);
				}
			}
			else if (handshakeType == MSG_FINISHED)
			{
				LOG_DEBUG("Processing Server Finished for client: %p", this);
				auto r = VerifyFinished(reader_sig);
				if (!r)
				{
					LOG_DEBUG("Failed to verify Finished for client: %p", this);
					return Result<VOID, Error>::Err(r, Error::Tls_OnPacketFailed);
				}
				LOG_DEBUG("Server Finished verified successfully for client: %p", this);
				crypto.UpdateHash(Span<const CHAR>(reader_sig.GetBuffer(), reader_sig.GetSize()));
				auto r2 = OnServerFinished();
				if (!r2)
				{
					LOG_DEBUG("Failed to process Server Finished for client: %p", this);
					return Result<VOID, Error>::Err(r2, Error::Tls_OnPacketFailed);
				}
				LOG_DEBUG("Server Finished processed successfully for client: %p", this);
			}
		}
		else if (packetType == CONTENT_CHANGECIPHERSPEC)
		{
		}
		else if (packetType == CONTENT_ALERT)
		{
			LOG_DEBUG("Processing Alert for client: %p", this);
			if (reader_sig.GetSize() >= 2)
			{
				[[maybe_unused]] INT32 level = reader_sig.Read<INT8>();
				[[maybe_unused]] INT32 code = reader_sig.Read<INT8>();
				LOG_ERROR("TLS Alert received for client: %p, level: %d, code: %d", this, level, code);
				return Result<VOID, Error>::Err(Error::Tls_OnPacketFailed);
			}
			LOG_DEBUG("TLS Alert received for client: %p, but buffer size is less than 2 bytes", this);
		}
		else if (packetType == CONTENT_APPLICATION_DATA)
		{
			LOG_DEBUG("Processing Application Data for client: %p, size: %d bytes", this, reader_sig.GetSize());
			channelBuffer.Append(Span<const CHAR>(reader_sig.GetBuffer(), reader_sig.GetSize()));
		}
		TlsReader.AdvanceReadPosition(seg_size);
	}

	return Result<VOID, Error>::Ok();
}

/// @brief Packet processing - read data from the socket, parse TLS packets
/// @return Result indicating success or Tls_ProcessReceiveFailed error
Result<VOID, Error> TlsClient::ProcessReceive()
{
	LOG_DEBUG("Processing received data for client: %p, current state index: %d", this, stateIndex);
	auto checkResult = recvBuffer.CheckSize(4096 * 4);
	if (!checkResult)
		return Result<VOID, Error>::Err(checkResult, Error::Tls_ProcessReceiveFailed);
	auto readResult = context.Read(Span<CHAR>(recvBuffer.GetBuffer() + recvBuffer.GetSize(), 4096 * 4));
	if (!readResult)
	{
		LOG_DEBUG("Failed to read data from socket for client: %p", this);
		(VOID)Close();
		return Result<VOID, Error>::Err(readResult, Error::Tls_ProcessReceiveFailed);
	}
	if (readResult.Value() <= 0)
	{
		LOG_DEBUG("Read returned 0 bytes from socket for client: %p", this);
		(VOID)Close();
		return Result<VOID, Error>::Err(Error::Tls_ProcessReceiveFailed);
	}
	INT64 len = readResult.Value();
	if (len > 0x7FFFFFFF)
		return Result<VOID, Error>::Err(Error::Tls_ProcessReceiveFailed);
	LOG_DEBUG("Received %lld bytes from socket for client: %p", len, this);
	recvBuffer.AppendSize((INT32)len);

	BinaryReader reader(Span<const UINT8>((const UINT8*)recvBuffer.GetBuffer(), (USIZE)recvBuffer.GetSize()));

	while (reader.Remaining() >= 5)
	{
		USIZE headerStart = reader.GetOffset();

		UINT8 contentType = reader.Read<UINT8>();
		UINT16 version = reader.Read<UINT16>(); // native byte order — matches OnPacket signature
		UINT16 packetSize = reader.ReadU16BE();

		if (reader.Remaining() < packetSize)
		{
			reader.SetOffset(headerStart);
			break;
		}

		LOG_DEBUG("Processing packet for client: %p, current index: %d, packet size: %d", this, (INT32)headerStart, packetSize);

		TlsBuffer unnamed(Span<CHAR>((PCHAR)reader.Current(), (USIZE)packetSize));

		auto ret = OnPacket(contentType, version, unnamed);
		if (!ret)
		{
			LOG_DEBUG("Failed to process packet for client: %p, current index: %d, packet size: %d", this, (INT32)headerStart, packetSize);
			(VOID)Close();
			return Result<VOID, Error>::Err(ret, Error::Tls_ProcessReceiveFailed);
		}
		LOG_DEBUG("Packet processed successfully for client: %p, current index: %d, packet size: %d", this, (INT32)headerStart, packetSize);

		reader.Skip(packetSize);
	}

	recvBuffer.Consume((INT32)reader.GetOffset());
	return Result<VOID, Error>::Ok();
}

/// @brief Read data from channel buffer
/// @param output Span wrapping the output buffer
/// @return Ok(bytes read) on success, or Err when channel is empty
Result<INT32, Error> TlsClient::ReadChannel(Span<CHAR> output)
{
	INT32 movesize = Math::Min((INT32)output.Size(), channelBuffer.GetSize() - channelBytesRead);
	LOG_DEBUG("Reading from channel for client: %p, requested size: %d, available size: %d, readed size: %d",
			  this, (INT32)output.Size(), channelBuffer.GetSize() - channelBytesRead, channelBytesRead);
	Memory::Copy(output.Data(), channelBuffer.GetBuffer() + channelBytesRead, movesize);
	channelBytesRead += movesize;
	if (((channelBytesRead > (channelBuffer.GetSize() >> 2) * 3) && (channelBuffer.GetSize() > 1024 * 1024)) || (channelBytesRead >= channelBuffer.GetSize()))
	{
		LOG_DEBUG("Clearing recv channel for client: %p, readed size: %d, total size: %d",
				  this, channelBytesRead, channelBuffer.GetSize());
		channelBuffer.Consume(channelBytesRead);
		channelBytesRead = 0;
	}
	LOG_DEBUG("Read %d bytes from channel for client: %p, new readed size: %d, total size: %d",
			  movesize, this, channelBytesRead, channelBuffer.GetSize());
	if (movesize == 0)
	{
		LOG_ERROR("recv channel size is 0, maybe error");
		return Result<INT32, Error>::Err(Error::Tls_ReadFailed_Channel);
	}
	LOG_DEBUG("Returning movesize: %d for client: %p", movesize, this);
	return Result<INT32, Error>::Ok(movesize);
}

/// @brief Open a TLS connection to the server, perform the TLS handshake by sending the ClientHello message and processing the server's responses
/// @return Result indicating whether the TLS connection was opened and the handshake completed successfully
Result<VOID, Error> TlsClient::Open()
{
	LOG_DEBUG("Connecting to host: %s for client: %p, secure: %d", host, this, secure);

	crypto.Reset();

	auto openResult = context.Open();
	if (!openResult)
	{
		LOG_DEBUG("Failed to connect to host: %s, for client: %p", host, this);
		return Result<VOID, Error>::Err(openResult, Error::Tls_OpenFailed_Socket);
	}
	LOG_DEBUG("Connected to host: %s, for client: %p", host, this);

	if (!secure)
	{
		LOG_DEBUG("Non-secure connection opened for client: %p", this);
		return Result<VOID, Error>::Ok();
	}

	auto helloResult = SendClientHello(host);
	if (!helloResult)
	{
		LOG_DEBUG("Failed to send Client Hello for client: %p", this);
		return Result<VOID, Error>::Err(helloResult, Error::Tls_OpenFailed_Handshake);
	}
	LOG_DEBUG("Client Hello sent successfully for client: %p", this);

	while (stateIndex < 6)
	{
		auto recvResult = ProcessReceive();
		if (!recvResult)
		{
			LOG_DEBUG("Failed to process received data for client: %p", this);
			return Result<VOID, Error>::Err(recvResult, Error::Tls_OpenFailed_Handshake);
		}
	}

	return Result<VOID, Error>::Ok();
}

/// @brief Close connection to the server, clean up buffers and cryptographic context
/// @return Result indicating whether the connection was closed successfully
Result<VOID, Error> TlsClient::Close()
{
	stateIndex = 0;
	channelBytesRead = 0;

	recvBuffer.Clear();
	channelBuffer.Clear();
	sendBuffer.Clear();
	if (secure)
	{
		crypto.Destroy();
	}

	LOG_DEBUG("Closing socket for client: %p", this);
	auto closeResult = context.Close();
	if (!closeResult)
	{
		return Result<VOID, Error>::Err(closeResult, Error::Tls_CloseFailed_Socket);
	}
	return Result<VOID, Error>::Ok();
}

/// @brief Write data to the TLS channel, encrypting it if the handshake is complete and the encoding is enabled
/// @param buffer Span containing the data to write to the TLS channel
/// @return Result with the number of bytes written, or an error
Result<UINT32, Error> TlsClient::Write(Span<const CHAR> buffer)
{
	UINT32 bufferLength = (UINT32)buffer.Size();
	LOG_DEBUG("Sending data for client: %p, size: %d bytes", this, bufferLength);

	if (!secure)
	{
		auto writeResult = context.Write(buffer);
		if (!writeResult)
		{
			return Result<UINT32, Error>::Err(writeResult, Error::Tls_WriteFailed_Send);
		}
		return Result<UINT32, Error>::Ok(writeResult.Value());
	}

	if (stateIndex < 6)
	{
		LOG_DEBUG("send error, state index is %d", stateIndex);
		return Result<UINT32, Error>::Err(Error::Tls_WriteFailed_NotReady);
	}

	sendBuffer.Clear();
	for (UINT32 i = 0; i < bufferLength;)
	{
		INT32 sendSize = Math::Min(bufferLength - i, 1024 * 16);
		auto setSizeResult = sendBuffer.SetSize(sendSize);
		if (!setSizeResult)
			return Result<UINT32, Error>::Err(setSizeResult, Error::Tls_WriteFailed_Send);
		Memory::Copy(sendBuffer.GetBuffer(), buffer.Data() + i, sendSize);
		auto sendResult = SendPacket(CONTENT_APPLICATION_DATA, 0x303, sendBuffer);
		if (!sendResult)
		{
			LOG_DEBUG("Failed to send packet for client: %p, size: %d bytes", this, sendSize);
			return Result<UINT32, Error>::Err(sendResult, Error::Tls_WriteFailed_Send);
		}

		i += sendSize;
	}

	LOG_DEBUG("Data sent successfully for client: %p, total size: %d bytes", this, bufferLength);
	return Result<UINT32, Error>::Ok(bufferLength);
}

/// @brief Read from the TLS channel, decrypting data if the handshake is complete and the encoding is enabled, and store it in the provided buffer
/// @param buffer Span where the read data will be stored
/// @return Result with the number of bytes read, or an error
Result<SSIZE, Error> TlsClient::Read(Span<CHAR> buffer)
{
	if (!secure)
	{
		auto readResult = context.Read(buffer);
		if (!readResult)
		{
			return Result<SSIZE, Error>::Err(readResult, Error::Tls_ReadFailed_Receive);
		}
		return Result<SSIZE, Error>::Ok(readResult.Value());
	}

	if (stateIndex < 6)
	{
		LOG_DEBUG("recv error, state index is %d", stateIndex);
		return Result<SSIZE, Error>::Err(Error::Tls_ReadFailed_NotReady);
	}
	LOG_DEBUG("Reading data for client: %p, requested size: %d", this, (INT32)buffer.Size());
	while (channelBuffer.GetSize() <= channelBytesRead)
	{
		auto recvResult = ProcessReceive();
		if (!recvResult)
		{
			LOG_DEBUG("recv error, maybe close socket");
			return Result<SSIZE, Error>::Err(recvResult, Error::Tls_ReadFailed_Receive);
		}
	}

	auto channelResult = ReadChannel(buffer);
	if (!channelResult)
		return Result<SSIZE, Error>::Err(channelResult, Error::Tls_ReadFailed_Channel);
	return Result<SSIZE, Error>::Ok((SSIZE)channelResult.Value());
}

/// @brief Factory method for TlsClient — creates and validates the underlying socket
/// @param host The hostname of the server to connect to
/// @param ipAddress The IP address of the server to connect to
/// @param port The port number of the server to connect to
/// @param secure Whether to use TLS handshake or plain TCP
/// @return Ok(TlsClient) on success, or Err with Tls_CreateFailed on failure
Result<TlsClient, Error> TlsClient::Create(PCCHAR host, const IPAddress &ipAddress, UINT16 port, BOOL secure)
{
	auto socketResult = Socket::Create(ipAddress, port);
	if (!socketResult)
		return Result<TlsClient, Error>::Err(socketResult, Error::Tls_CreateFailed);

	TlsClient client(host, ipAddress, static_cast<Socket &&>(socketResult.Value()), secure);
	return Result<TlsClient, Error>::Ok(static_cast<TlsClient &&>(client));
}
