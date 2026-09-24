#include "lib/network/websocket/websocket_client.h"
#include "core/containers/buffer.h"
#include "core/memory/memory.h"
#include "core/string/string.h"
#include "platform/system/random.h"
#include "platform/console/logger.h"
#include "lib/network/dns/dns_client.h"
#include "lib/network/http/http_client.h"

/**
 * @brief Performs the WebSocket opening handshake (RFC 6455 Section 4)
 * @details Sends the HTTP Upgrade request with Sec-WebSocket-Key (16 random bytes,
 * Base64-encoded per Section 4.1) and validates the server responds with HTTP 101.
 * Any extraHeaders span (CRLF-terminated lines) is written verbatim between the
 * standard headers and the blank line — the agent's identity headers ride here.
 * Falls back to IPv4 if the initial IPv6 connection attempt fails.
 * @see https://datatracker.ietf.org/doc/html/rfc6455#section-4
 */
Result<VOID, Error> WebSocketClient::Open(PCCHAR path, Span<const CHAR> extraHeaders)
{
	BOOL isSecure = tlsContext.IsSecure();
	LOG_DEBUG("Opening WebSocket client to %s:%u%s (secure: %s)", hostName, port, path, isSecure ? "true" : "false");

	auto openResult = tlsContext.Open();

	if (!openResult && ipAddress.IsIPv6())
	{
		LOG_DEBUG("Failed to open network transport for WebSocket client using IPv6 address, attempting IPv4 fallback");

		auto dnsResult = DnsClient::Resolve(Span<const CHAR>(hostName, StringUtils::Length(hostName)), DnsRecordType::A);
		if (!dnsResult)
		{
			LOG_ERROR("Failed to resolve IPv4 address for %s, cannot connect to WebSocket server (error: %e)", hostName, dnsResult.Error());
			return Result<VOID, Error>::Err(dnsResult, Error::Ws_DnsFailed);
		}

		ipAddress = dnsResult.Value();

		(VOID)tlsContext.Close();
		auto tlsResult = TlsClient::Create(hostName, ipAddress, port, isSecure);
		if (!tlsResult)
		{
			LOG_ERROR("Failed to create TLS client for IPv4 fallback (error: %e)", tlsResult.Error());
			return Result<VOID, Error>::Err(tlsResult, Error::Ws_TransportFailed);
		}
		tlsContext = static_cast<TlsClient &&>(tlsResult.Value());
		openResult = tlsContext.Open();
	}

	if (!openResult)
	{
		LOG_DEBUG("Failed to open network transport for WebSocket client (error: %e)", openResult.Error());
		return Result<VOID, Error>::Err(openResult, Error::Ws_TransportFailed);
	}

	// RFC 6455 Section 4.1: Sec-WebSocket-Key is 16 random bytes, Base64-encoded (24 chars)
	UINT32 key[4];
	Random random;
	for (INT32 i = 0; i < 4; i++)
		key[i] = (UINT32)random.Get();

	CHAR secureKey[25]; // Base64 of 16 bytes = 24 chars + null
	Base64::Encode(Span<const CHAR>((PCCHAR)key, 16), Span<CHAR>(secureKey));

	auto writeStr = [&](PCCHAR s) -> BOOL
	{
		UINT32 len = StringUtils::Length(s);
		auto r = tlsContext.Write(Span<const CHAR>(s, len));
		if (!r)
		{
			LOG_ERROR("Failed to write WebSocket upgrade request segment to %s:%u (error: %e)", hostName, port, r.Error());
			return false;
		}
		return r.Value() == len;
	};

	if (!writeStr("GET ") ||
		!writeStr(path) ||
		!writeStr(" HTTP/1.1\r\nHost: ") ||
		!writeStr(hostName) ||
		!writeStr("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ") ||
		!writeStr(secureKey) ||
		!writeStr("\r\nSec-WebSocket-Version: 13\r\nOrigin: ") ||
		!writeStr(isSecure ? "https://" : "http://") ||
		!writeStr(hostName) ||
		!writeStr("\r\n"))
	{
		(VOID)Close();
		return Result<VOID, Error>::Err(Error::Ws_WriteFailed);
	}

	// Extra headers (identity) — written as their own span since they are not
	// null-terminated.
	if (extraHeaders.Size() > 0)
	{
		auto extraWrite = tlsContext.Write(extraHeaders);
		if (!extraWrite || extraWrite.Value() != extraHeaders.Size())
		{
			LOG_ERROR("Failed to write identity headers during WebSocket upgrade to %s:%u", hostName, port);
			(VOID)Close();
			return Result<VOID, Error>::Err(Error::Ws_WriteFailed);
		}
	}

	if (!writeStr("\r\n"))
	{
		(VOID)Close();
		return Result<VOID, Error>::Err(Error::Ws_WriteFailed);
	}

	auto headerResult = HttpClient::ReadResponseHeaders(tlsContext, 101);
	if (!headerResult)
	{
		LOG_ERROR("WebSocket upgrade to %s:%u rejected: no HTTP 101 from server (error: %e)", hostName, port, headerResult.Error());
		(VOID)Close();
		return Result<VOID, Error>::Err(headerResult, Error::Ws_HandshakeFailed);
	}

	isConnected = true;
	return Result<VOID, Error>::Ok();
}

/**
 * @brief Sends a Close frame with status 1000 (Normal Closure) and tears down the transport
 * @details Implements RFC 6455 Section 7.1.1 — the client initiates the closing handshake
 * by sending a Close frame whose payload is the 2-byte status code in network byte order.
 * @see https://datatracker.ietf.org/doc/html/rfc6455#section-7
 * @see https://datatracker.ietf.org/doc/html/rfc6455#section-7.4.1
 */
Result<VOID, Error> WebSocketClient::Close()
{
	if (isConnected)
	{
		// RFC 6455 Section 5.5.1: Send Close frame with status code 1000 (Normal Closure, big-endian)
		UINT16 statusCode = ByteOrder::Swap16(1000);
		(VOID)Write(Span<const CHAR>((const CHAR *)&statusCode, sizeof(statusCode)), WebSocketOpcode::Close);
	}

	isConnected = false;
	(VOID)tlsContext.Close();
	LOG_DEBUG("WebSocket client to %s:%u closed", hostName, port);
	return Result<VOID, Error>::Ok();
}

/**
 * @brief Constructs and sends a masked WebSocket frame (RFC 6455 Section 5.2, 5.3)
 * @details Builds the frame header with FIN=1 and the appropriate payload length encoding
 * (7-bit / 16-bit / 64-bit). Generates a random 32-bit masking key and XOR-masks the
 * entire payload — client-to-server frames MUST be masked per Section 5.1. The masked
 * frame is handed to the transport in a single write so large payloads cross TLS as
 * full 16 KiB records instead of one tiny record per masked chunk.
 * @see https://datatracker.ietf.org/doc/html/rfc6455#section-5.2
 * @see https://datatracker.ietf.org/doc/html/rfc6455#section-5.3
 */
Result<UINT32, Error> WebSocketClient::Write(Span<const CHAR> buffer, WebSocketOpcode opcode)
{
	return WritePayload(Span<const CHAR>(), buffer, opcode);
}

/**
 * @brief Sends a command response frame with the correlation id spliced in
 * @param status First UINT32 of the payload (the handler's status code)
 * @param correlationId Correlation id spliced between status and body
 * @param body Response body (everything after the status UINT32)
 * @param opcode Frame opcode (default: Binary)
 * @return Ok(payload bytes sent) on success, Err on failure
 *
 * @details Produces the same wire bytes as Write() over `[status][corrId][body]`
 * but assembles them inside the transport's masked scratch buffer, so the
 * caller's separate splice allocation and full-payload copy are skipped.
 */
Result<UINT32, Error> WebSocketClient::WriteResponse(UINT32 status, UINT32 correlationId, Span<const CHAR> body, WebSocketOpcode opcode)
{
	UINT8 prefix[8];
	Memory::Copy(prefix, &status, sizeof(status));
	Memory::Copy(prefix + sizeof(status), &correlationId, sizeof(correlationId));
	return WritePayload(Span<const CHAR>((PCHAR)prefix, sizeof(prefix)), body, opcode);
}

// XOR-mask size bytes continuing the mask phase at payload offset `phase`
// (RFC 6455 Section 5.3 — 4 bytes per iteration in the main loop)
static VOID MaskSpan(UINT8 *dst, const UINT8 *src, USIZE size, USIZE phase, const UINT8 *maskKey)
{
	USIZE i = 0;
	for (; i + 4 <= size; i += 4)
	{
		dst[i] = src[i] ^ maskKey[(phase + i) & 3];
		dst[i + 1] = src[i + 1] ^ maskKey[(phase + i + 1) & 3];
		dst[i + 2] = src[i + 2] ^ maskKey[(phase + i + 2) & 3];
		dst[i + 3] = src[i + 3] ^ maskKey[(phase + i + 3) & 3];
	}
	for (; i < size; i++)
		dst[i] = src[i] ^ maskKey[(phase + i) & 3];
}

/**
 * @brief Shared framing path: builds one masked frame over prefix + body
 * @return Ok(payload bytes sent) on success, Err(Ws_WriteFailed | Ws_NotConnected) on failure
 *
 * @details The payload is prefix followed by body (either may be empty). Small
 * payloads are masked into a stack chunk and written once; large payloads are
 * masked into a heap scratch buffer (header included) and written once. The
 * mask phase runs over the concatenated payload per RFC 6455 Section 5.3.
 */
Result<UINT32, Error> WebSocketClient::WritePayload(Span<const CHAR> prefix, Span<const CHAR> body, WebSocketOpcode opcode)
{
	if (!isConnected && opcode != WebSocketOpcode::Close)
	{
		return Result<UINT32, Error>::Err(Error::Ws_NotConnected);
	}

	USIZE payloadSize = prefix.Size() + body.Size();

	// Build frame header on stack (max 14 bytes: 2 base + 8 ext length + 4 mask key)
	UINT8 header[14];
	UINT32 headerLength;

	// RFC 6455 Section 5.2: byte 0 = FIN (bit 7) | opcode (bits 0-3)
	header[0] = (UINT8)((UINT8)opcode | 0x80);

	// Generate masking key from a single random value
	Random random;
	UINT32 maskKeyVal = (UINT32)random.Get();
	PUINT8 maskKey = (PUINT8)&maskKeyVal;

	// RFC 6455 Section 5.2: byte 1 = MASK (bit 7) | payload length (bits 0-6)
	if (payloadSize <= 125)
	{
		header[1] = (UINT8)(payloadSize | 0x80);
		Memory::Copy(header + 2, maskKey, 4);
		headerLength = 6;
	}
	else if (payloadSize <= 0xFFFF)
	{
		header[1] = (126 | 0x80);
		UINT16 len16 = ByteOrder::Swap16((UINT16)payloadSize);
		Memory::Copy(header + 2, &len16, 2);
		Memory::Copy(header + 4, maskKey, 4);
		headerLength = 8;
	}
	else
	{
		header[1] = (127 | 0x80);
		UINT64 len64 = ByteOrder::Swap64((UINT64)payloadSize);
		Memory::Copy(header + 2, &len64, 8);
		Memory::Copy(header + 10, maskKey, 4);
		headerLength = 14;
	}

	// Chunk buffer for masking: small enough for the stack, multiple of 4 for mask alignment
	UINT8 chunk[256];

	// Small frames: combine header + masked payload into a single write
	if (payloadSize <= sizeof(chunk) - headerLength)
	{
		Memory::Copy(chunk, header, headerLength);
		UINT8 *dst = chunk + headerLength;
		MaskSpan(dst, (const UINT8 *)prefix.Data(), prefix.Size(), 0, maskKey);
		MaskSpan(dst + prefix.Size(), (const UINT8 *)body.Data(), body.Size(), prefix.Size(), maskKey);

		UINT32 frameLength = headerLength + (UINT32)payloadSize;
		auto smallWrite = tlsContext.Write(Span<const CHAR>((PCHAR)chunk, frameLength));
		if (!smallWrite)
			return Result<UINT32, Error>::Err(smallWrite, Error::Ws_WriteFailed);
		if (smallWrite.Value() != frameLength)
			return Result<UINT32, Error>::Err(Error::Ws_WriteFailed);

		return Result<UINT32, Error>::Ok((UINT32)payloadSize);
	}

	// Large frames: mask header + payload into one scratch buffer so the single
	// transport write splits into full-size TLS records at the TLS layer
	Buffer<CHAR> masked;
	if (!masked.Init((USIZE)headerLength + payloadSize))
		return Result<UINT32, Error>::Err(Error::Ws_WriteFailed);
	Memory::Copy(masked.Data, header, headerLength);

	PUINT8 dst = (PUINT8)masked.Data + headerLength;
	MaskSpan(dst, (const UINT8 *)prefix.Data(), prefix.Size(), 0, maskKey);
	MaskSpan(dst + prefix.Size(), (const UINT8 *)body.Data(), body.Size(), prefix.Size(), maskKey);

	UINT32 frameLength = headerLength + (UINT32)payloadSize;
	auto frameWrite = tlsContext.Write(Span<const CHAR>(masked.Data, frameLength));
	if (!frameWrite)
		return Result<UINT32, Error>::Err(frameWrite, Error::Ws_WriteFailed);
	if (frameWrite.Value() != frameLength)
		return Result<UINT32, Error>::Err(Error::Ws_WriteFailed);

	return Result<UINT32, Error>::Ok((UINT32)payloadSize);
}

/**
 * @brief Reads exactly buffer.Size() bytes from the TLS transport
 * @details Loops over TlsClient::Read until all requested bytes are received.
 * Returns Err immediately if any individual read returns an error or zero bytes.
 * Used by ReceiveFrame to read fixed-size frame header fields and payload data.
 */
Result<VOID, Error> WebSocketClient::ReceiveRestrict(Span<CHAR> buffer)
{
	USIZE totalBytesRead = 0;
	while (totalBytesRead < buffer.Size())
	{
		auto readResult = tlsContext.Read(Span<CHAR>(buffer.Data() + totalBytesRead, buffer.Size() - totalBytesRead));
		if (!readResult)
			return Result<VOID, Error>::Err(readResult, Error::Ws_ReceiveFailed);
		if (readResult.Value() <= 0)
			return Result<VOID, Error>::Err(Error::Ws_ReceiveFailed);
		totalBytesRead += readResult.Value();
	}
	return Result<VOID, Error>::Ok();
}

/**
 * @brief Applies the RFC 6455 Section 5.3 XOR masking transformation in-place
 * @details Iterates over frame.Data applying: data[i] ^= maskKey[i % 4].
 * Processes 4 bytes per iteration in the main loop, then handles 0–3 trailing bytes.
 * The same function both masks and unmasks since XOR is self-inverse.
 * @see https://datatracker.ietf.org/doc/html/rfc6455#section-5.3
 */
VOID WebSocketClient::MaskFrame(WebSocketFrame &frame, UINT32 maskKey)
{
	PUINT8 mask = (PUINT8)&maskKey;
	PUINT8 d = (PUINT8)frame.Data;
	UINT32 len = (UINT32)frame.Length;

	// Process 4 bytes at a time (unrolled, no modulo in main loop)
	UINT32 i = 0;
	for (; i + 4 <= len; i += 4)
	{
		d[i] ^= mask[0];
		d[i + 1] ^= mask[1];
		d[i + 2] ^= mask[2];
		d[i + 3] ^= mask[3];
	}

	// Remaining 0-3 bytes
	for (; i < len; i++)
		d[i] ^= mask[i & 3];
}

/**
 * @brief Reads and parses a single WebSocket frame from the transport (RFC 6455 Section 5.2)
 * @details Parses the wire format:
 *   Byte 0: [FIN:1][RSV1:1][RSV2:1][RSV3:1][opcode:4]
 *   Byte 1: [MASK:1][payload_len:7]
 *   If payload_len == 126: next 2 bytes are the 16-bit length (network byte order)
 *   If payload_len == 127: next 8 bytes are the 64-bit length (network byte order)
 *   If MASK == 1: next 4 bytes are the masking key
 *   Remaining bytes: payload data (unmasked after reading if MASK was set)
 *
 * Rejects frames with non-zero RSV bits (Section 5.2) and payloads > 64 MB.
 * @see https://datatracker.ietf.org/doc/html/rfc6455#section-5.2
 */
Result<VOID, Error> WebSocketClient::ReceiveFrame(WebSocketFrame &frame)
{
	UINT8 header[2] = {0};
	auto headerResult = ReceiveRestrict(Span<CHAR>((PCHAR)header, sizeof(header)));
	if (!headerResult)
		return Result<VOID, Error>::Err(headerResult, Error::Ws_ReceiveFailed);

	UINT8 b1 = header[0];
	UINT8 b2 = header[1];

	frame.Fin = (b1 >> 7) & 1;
	frame.Rsv1 = (b1 >> 6) & 1;
	frame.Rsv2 = (b1 >> 5) & 1;
	frame.Rsv3 = (b1 >> 4) & 1;
	frame.Opcode = (WebSocketOpcode)(b1 & 0x0F);
	frame.Mask = (b2 >> 7) & 1;

	// RFC 6455 Section 5.2: RSV1-3 MUST be 0 unless an extension defining their meaning is negotiated
	if (frame.Rsv1 || frame.Rsv2 || frame.Rsv3)
		return Result<VOID, Error>::Err(Error::Ws_InvalidFrame);

	UINT8 lengthBits = b2 & 0x7F;

	if (lengthBits == 126)
	{
		UINT16 len16 = 0;
		auto lenResult = ReceiveRestrict(Span<CHAR>((PCHAR)&len16, sizeof(len16)));
		if (!lenResult)
			return Result<VOID, Error>::Err(lenResult, Error::Ws_ReceiveFailed);
		frame.Length = ByteOrder::Swap16(len16);
	}
	else if (lengthBits == 127)
	{
		UINT64 len64 = 0;
		auto lenResult = ReceiveRestrict(Span<CHAR>((PCHAR)&len64, sizeof(len64)));
		if (!lenResult)
			return Result<VOID, Error>::Err(lenResult, Error::Ws_ReceiveFailed);
		frame.Length = ByteOrder::Swap64(len64);
	}
	else
	{
		frame.Length = lengthBits;
	}

	// Reject frames that would require an absurd allocation (>64 MB)
	if (frame.Length > 0x4000000)
		return Result<VOID, Error>::Err(Error::Ws_FrameTooLarge);

	UINT32 frameMask = 0;
	if (frame.Mask)
	{
		auto maskResult = ReceiveRestrict(Span<CHAR>((PCHAR)&frameMask, sizeof(frameMask)));
		if (!maskResult)
			return Result<VOID, Error>::Err(maskResult, Error::Ws_ReceiveFailed);
	}

	frame.Data = nullptr;
	if (frame.Length > 0)
	{
		frame.Data = new CHAR[(USIZE)frame.Length];
		if (!frame.Data)
			return Result<VOID, Error>::Err(Error::Ws_AllocFailed);

		auto dataResult = ReceiveRestrict(Span<CHAR>(frame.Data, (USIZE)frame.Length));
		if (!dataResult)
		{
			delete[] frame.Data;
			frame.Data = nullptr;
			return Result<VOID, Error>::Err(dataResult, Error::Ws_ReceiveFailed);
		}
	}

	if (frame.Mask && frame.Data)
		MaskFrame(frame, frameMask);

	return Result<VOID, Error>::Ok();
}

/**
 * @brief Reads the next complete WebSocket message, reassembling fragmented frames
 * @details Implements message reception per RFC 6455 Section 5.4 (Fragmentation):
 *   - An unfragmented message is a single frame with FIN=1 and opcode != 0
 *   - A fragmented message starts with opcode != 0 and FIN=0, followed by zero or more
 *     continuation frames (opcode=0, FIN=0), ending with a continuation frame with FIN=1
 *   - Payloads from all fragments are concatenated into a single WebSocketMessage
 *
 * Control frames (Close, Ping, Pong) may be interleaved between data fragments:
 *   - Close (Section 5.5.1): echoes the status code and returns Err(Ws_ConnectionClosed)
 *   - Ping (Section 5.5.2): responds with Pong carrying the same Application Data
 *   - Pong (Section 5.5.3): silently discarded (unsolicited pongs are allowed)
 *
 * @see https://datatracker.ietf.org/doc/html/rfc6455#section-5.4
 * @see https://datatracker.ietf.org/doc/html/rfc6455#section-5.5
 */
Result<WebSocketMessage, Error> WebSocketClient::Read()
{
	if (!isConnected)
	{
		return Result<WebSocketMessage, Error>::Err(Error::Ws_NotConnected);
	}

	WebSocketFrame frame;
	WebSocketMessage message;
	BOOL messageComplete = false;

	while (isConnected)
	{
		frame = WebSocketFrame();
		auto frameResult = ReceiveFrame(frame);
		if (!frameResult)
			break;

		if (frame.Opcode == WebSocketOpcode::Text || frame.Opcode == WebSocketOpcode::Binary || frame.Opcode == WebSocketOpcode::Continue)
		{
			if (frame.Opcode == WebSocketOpcode::Continue && message.Data == nullptr)
			{
				delete[] frame.Data;
				frame.Data = nullptr;
				break;
			}

			// Capture opcode from the initial (non-continuation) frame
			if (frame.Opcode != WebSocketOpcode::Continue)
				message.Opcode = frame.Opcode;

			if (frame.Length > 0)
			{
				if (message.Data)
				{
					// Reassemble the fragments into a growable buffer, then hand
					// ownership of the exact accumulated array to the message's
					// out-param fields (the caller deletes[] message.Data).
					Buffer<CHAR> assembled;
					if (!assembled.Init(message.Length) ||
						!assembled.Append(Span<const CHAR>(message.Data, message.Length)) ||
						!assembled.Append(Span<const CHAR>(frame.Data, (USIZE)frame.Length)))
					{
						delete[] frame.Data;
						frame.Data = nullptr;
						delete[] message.Data;
						message.Data = nullptr;
						break;
					}
					USIZE assembledLength = assembled.Size;
					delete[] message.Data;
					message.Data = assembled.Release();
					message.Length = assembledLength;
					delete[] frame.Data;
					frame.Data = nullptr;
				}
				else
				{
					message.Data = frame.Data;
					frame.Data = nullptr;
					message.Length = (USIZE)frame.Length;
				}
			}

			if (frame.Fin)
			{
				messageComplete = true;
				break;
			}
		}
		else if (frame.Opcode == WebSocketOpcode::Close)
		{
			// RFC 6455 Section 5.5.1: echo the 2-byte status code back in the Close response
			(VOID)Write(Span<const CHAR>(frame.Data, (frame.Length >= 2) ? 2 : 0), WebSocketOpcode::Close);
			delete[] frame.Data;
			frame.Data = nullptr;
			isConnected = false;
			return Result<WebSocketMessage, Error>::Err(Error::Ws_ConnectionClosed);
		}
		else if (frame.Opcode == WebSocketOpcode::Ping)
		{
			(VOID)Write(Span<const CHAR>(frame.Data, (UINT32)frame.Length), WebSocketOpcode::Pong);
			delete[] frame.Data;
			frame.Data = nullptr;
		}
		else if (frame.Opcode == WebSocketOpcode::Pong)
		{
			delete[] frame.Data;
			frame.Data = nullptr;
		}
		else
		{
			delete[] frame.Data;
			frame.Data = nullptr;
			break;
		}
	}

	if (!messageComplete)
	{
		return Result<WebSocketMessage, Error>::Err(Error::Ws_ReceiveFailed);
	}

	return Result<WebSocketMessage, Error>::Ok(static_cast<WebSocketMessage &&>(message));
}

/**
 * @brief Factory method — creates and connects a WebSocketClient from a ws:// or wss:// URL
 * @param url WebSocket URL (ws:// or wss://)
 * @return Ok(WebSocketClient) in the OPEN state, or Err on parse/DNS/TLS/handshake failure
 *
 * @details Performs the full connection sequence:
 *   1. Parses the URL into host, path, port, and secure flag via HttpClient::ParseUrl
 *   2. Resolves the hostname to an IP address via DnsClient::Resolve (AAAA first, A fallback)
 *   3. Creates the TLS transport via TlsClient::Create (with IPv4 fallback on IPv6 failure)
 *   4. Performs the WebSocket opening handshake (RFC 6455 Section 4)
 *
 * The path component lives only on this function's stack frame and is not stored
 * in the returned client, keeping the object size small.
 *
 * @see https://datatracker.ietf.org/doc/html/rfc6455#section-3
 * @see https://datatracker.ietf.org/doc/html/rfc6455#section-4
 */
Result<WebSocketClient, Error> WebSocketClient::Create(Span<const CHAR> url, Span<const CHAR> extraHeaders)
{
	CHAR host[254];
	CHAR parsedPath[2048];
	UINT16 port;
	BOOL isSecure = false;
	auto parseResult = HttpClient::ParseUrl(url, host, parsedPath, port, isSecure);
	if (!parseResult)
	{
		LOG_ERROR("Invalid WebSocket URL (error: %e)", parseResult.Error());
		return Result<WebSocketClient, Error>::Err(parseResult, Error::Ws_CreateFailed);
	}

	Span<const CHAR> hostSpan(host, StringUtils::Length(host));
	auto dnsResult = DnsClient::Resolve(hostSpan);
	if (!dnsResult)
	{
		LOG_ERROR("Failed to resolve hostname %s (error: %e)", host, dnsResult.Error());
		return Result<WebSocketClient, Error>::Err(dnsResult, Error::Ws_CreateFailed);
	}
	auto& ip = dnsResult.Value();

	auto tlsResult = TlsClient::Create(host, ip, port, isSecure);

	// IPv6 socket creation can fail on platforms without IPv6 support (e.g. UEFI)
	if (!tlsResult && ip.IsIPv6())
	{
		auto dnsResultV4 = DnsClient::Resolve(hostSpan, DnsRecordType::A);
		if (dnsResultV4)
		{
			ip = dnsResultV4.Value();
			tlsResult = TlsClient::Create(host, ip, port, isSecure);
		}
	}

	if (!tlsResult)
	{
		LOG_ERROR("Failed to create TCP/TLS transport to %s:%u (error: %e)", host, port, tlsResult.Error());
		return Result<WebSocketClient, Error>::Err(tlsResult, Error::Ws_CreateFailed);
	}

	WebSocketClient client(host, ip, port, static_cast<TlsClient &&>(tlsResult.Value()));

	auto openResult = client.Open(parsedPath, extraHeaders);
	if (!openResult)
		return Result<WebSocketClient, Error>::Err(openResult, Error::Ws_CreateFailed);

	return Result<WebSocketClient, Error>::Ok(static_cast<WebSocketClient &&>(client));
}
