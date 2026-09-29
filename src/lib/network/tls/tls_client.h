#pragma once

#include "platform/platform.h"
#include "lib/network/tls/tls_buffer.h"
#include "lib/network/tls/tls_cipher.h"

// TLS state
struct TlsState
{
	INT32 contentType;   // TLS content type
	INT32 handshakeType; // TLS handshake type
};

class TlsClient final
{
private:
	PCCHAR host;
	IPAddress ip;
	Socket context;
	TlsCipher crypto;
	BOOL secure;             // Whether to use TLS handshake or plain TCP
	INT32 stateIndex;        // Current state index
	TlsBuffer sendBuffer;    // Send buffer
	TlsBuffer recvBuffer;    // Receive buffer
	TlsBuffer channelBuffer; // Channel buffer for received data
	INT32 channelBytesRead;  // Number of bytes read from channel buffer
	UINT64 lastEncryptNs;    // Last Write's TLS-layer time excl. syscalls (record staging + encryption)
	UINT64 lastSocketWriteNs;// Last Write's socket send syscall time
	[[nodiscard]] Result<INT32, Error> ReadChannel(Span<CHAR> output);
	[[nodiscard]] Result<VOID, Error> ProcessReceive();
	[[nodiscard]] Result<VOID, Error> OnPacket(INT32 packetType, INT32 version, TlsBuffer &TlsReader);
	[[nodiscard]] Result<VOID, Error> OnServerFinished();
	[[nodiscard]] Result<VOID, Error> VerifyFinished(TlsBuffer &TlsReader);
	[[nodiscard]] Result<VOID, Error> OnServerHelloDone();
	[[nodiscard]] Result<VOID, Error> OnServerHello(TlsBuffer &TlsReader);
	[[nodiscard]] Result<VOID, Error> SendChangeCipherSpec();
	[[nodiscard]] Result<VOID, Error> SendClientExchange();
	[[nodiscard]] Result<VOID, Error> SendClientFinished();
	[[nodiscard]] Result<VOID, Error> SendClientHello(const CHAR *host);
	[[nodiscard]] Result<VOID, Error> SendPacket(INT32 packetType, INT32 ver, TlsBuffer &TlsBuffer);

	// Private trivial constructor — only used by Create()
	TlsClient(PCCHAR host, const IPAddress &ipAddress, Socket &&socket, BOOL secure)
		: host(host), ip(ipAddress), context(static_cast<Socket &&>(socket)), secure(secure), stateIndex(0), channelBytesRead(0), lastEncryptNs(0), lastSocketWriteNs(0) {}

public:
	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	// Placement new required by Result<TlsClient, Error>
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}
	TlsClient() : host(nullptr), ip(), secure(true), stateIndex(0), channelBytesRead(0), lastEncryptNs(0), lastSocketWriteNs(0) {}

	// Factory — caller MUST check the result (enforced by [[nodiscard]])
	[[nodiscard]] static Result<TlsClient, Error> Create(PCCHAR host, const IPAddress &ipAddress, UINT16 port, BOOL secure = true);

	// Destructor
	~TlsClient()
	{
		if (IsValid())
			(VOID)Close();
	}

	// Disallow copy construction and assignment
	TlsClient(const TlsClient &) = delete;
	TlsClient &operator=(const TlsClient &) = delete;

	// Move semantics
	TlsClient(TlsClient &&other) noexcept
		: host(other.host), ip(other.ip), context(static_cast<Socket &&>(other.context)), crypto(static_cast<TlsCipher &&>(other.crypto)), secure(other.secure), stateIndex(other.stateIndex), sendBuffer(static_cast<TlsBuffer &&>(other.sendBuffer)), recvBuffer(static_cast<TlsBuffer &&>(other.recvBuffer)), channelBuffer(static_cast<TlsBuffer &&>(other.channelBuffer)), channelBytesRead(other.channelBytesRead), lastEncryptNs(other.lastEncryptNs), lastSocketWriteNs(other.lastSocketWriteNs)
	{
		other.host = nullptr;
		other.secure = true;
		other.stateIndex = 0;
		other.channelBytesRead = 0;
		other.lastEncryptNs = 0;
		other.lastSocketWriteNs = 0;
	}

	TlsClient &operator=(TlsClient &&other) noexcept
	{
		if (this != &other)
		{
			if (IsValid())
				(VOID)Close();
			host = other.host;
			ip = other.ip;
			context = static_cast<Socket &&>(other.context);
			crypto = static_cast<TlsCipher &&>(other.crypto);
			secure = other.secure;
			stateIndex = other.stateIndex;
			sendBuffer = static_cast<TlsBuffer &&>(other.sendBuffer);
			recvBuffer = static_cast<TlsBuffer &&>(other.recvBuffer);
			channelBuffer = static_cast<TlsBuffer &&>(other.channelBuffer);
			channelBytesRead = other.channelBytesRead;
			lastEncryptNs = other.lastEncryptNs;
			lastSocketWriteNs = other.lastSocketWriteNs;
			other.host = nullptr;
			other.secure = true;
			other.stateIndex = 0;
			other.channelBytesRead = 0;
			other.lastEncryptNs = 0;
			other.lastSocketWriteNs = 0;
		}
		return *this;
	}

	// Accessors and operations
	BOOL IsValid() const { return context.IsValid(); }
	BOOL IsSecure() const { return secure; }
	/** @brief Nanoseconds the last Write spent in the TLS layer excl. syscalls (record staging + encryption; 0 on plaintext) */
	UINT64 GetLastEncryptNs() const { return lastEncryptNs; }
	/** @brief Nanoseconds the last Write spent in socket send syscalls */
	UINT64 GetLastSocketWriteNs() const { return lastSocketWriteNs; }
	[[nodiscard]] Result<VOID, Error> Open();
	[[nodiscard]] Result<VOID, Error> Close();
	[[nodiscard]] Result<SSIZE, Error> Read(Span<CHAR> buffer);
	[[nodiscard]] Result<UINT32, Error> Write(Span<const CHAR> buffer);
};
