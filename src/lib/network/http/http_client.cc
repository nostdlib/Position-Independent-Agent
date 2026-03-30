#include "lib/network/http/http_client.h"
#include "lib/network/dns/dns_client.h"
#include "platform/console/logger.h"

// Helper to append a null-terminated string to a buffer
static USIZE AppendStr(Span<CHAR> buf, USIZE pos, const CHAR *str) noexcept
{
	for (USIZE i = 0; str[i] != '\0' && pos < buf.Size(); i++)
	{
		buf[pos++] = str[i];
	}
	return pos;
}

/// @brief Factory method for HttpClient — creates from URL with DNS resolution
/// @param url URL of the server to connect to (IP address will be resolved from the hostname)
/// @return Ok(HttpClient) on success, or Err with Http_CreateFailed on failure

Result<HttpClient, Error> HttpClient::Create(Span<const CHAR> url)
{
	CHAR host[254];
	CHAR parsedPath[2048];
	UINT16 port;
	BOOL isSecure = false;
	auto parseResult = ParseUrl(url, host, parsedPath, port, isSecure);
	if (!parseResult)
		return Result<HttpClient, Error>::Err(parseResult, Error::Http_CreateFailed);

	auto dnsResult = DnsClient::Resolve(Span<const CHAR>(host, StringUtils::Length(host)));
	if (!dnsResult)
	{
		LOG_ERROR("Failed to resolve hostname %s (error: %e)", host, dnsResult.Error());
		return Result<HttpClient, Error>::Err(dnsResult, Error::Http_CreateFailed);
	}
	auto& ip = dnsResult.Value();

	auto tlsResult = TlsClient::Create(host, ip, port, isSecure);

	// IPv6 socket creation can fail on platforms without IPv6 support (e.g. UEFI)
	if (!tlsResult && ip.IsIPv6())
	{
		auto dnsResultV4 = DnsClient::Resolve(Span<const CHAR>(host, StringUtils::Length(host)), DnsRecordType::A);
		if (dnsResultV4)
		{
			ip = dnsResultV4.Value();
			tlsResult = TlsClient::Create(host, ip, port, isSecure);
		}
	}

	if (!tlsResult)
		return Result<HttpClient, Error>::Err(tlsResult, Error::Http_CreateFailed);

	HttpClient client(ip, port, static_cast<TlsClient &&>(tlsResult.Value()));
	return Result<HttpClient, Error>::Ok(static_cast<HttpClient &&>(client));
}

/// @brief Open a connection to the server
/// @return Ok on success, or Err with Http_OpenFailed on failure

Result<VOID, Error> HttpClient::Open()
{
	auto r = tlsContext.Open();
	if (!r)
		return Result<VOID, Error>::Err(r, Error::Http_OpenFailed);
	return Result<VOID, Error>::Ok();
}

/// @brief Closes the connection to the server and cleans up resources
/// @return Ok on success, or Err with Http_CloseFailed on failure

Result<VOID, Error> HttpClient::Close()
{
	auto r = tlsContext.Close();
	if (!r)
		return Result<VOID, Error>::Err(r, Error::Http_CloseFailed);
	return Result<VOID, Error>::Ok();
}

/// @brief Read data from the server into the provided buffer, handling decryption if the connection is secure
/// @param buffer The buffer to store the read data
/// @return Ok(bytesRead) on success, or Err with Http_ReadFailed on failure

Result<SSIZE, Error> HttpClient::Read(Span<CHAR> buffer)
{
	auto r = tlsContext.Read(buffer);
	if (!r)
		return Result<SSIZE, Error>::Err(r, Error::Http_ReadFailed);
	return Result<SSIZE, Error>::Ok(r.Value());
}

/// @brief Write data to the server
/// @param buffer The data to be sent to the server
/// @return Ok(bytesWritten) on success, or Err with Http_WriteFailed on failure

Result<UINT32, Error> HttpClient::Write(Span<const CHAR> buffer)
{
	auto r = tlsContext.Write(buffer);
	if (!r)
		return Result<UINT32, Error>::Err(r, Error::Http_WriteFailed);
	return Result<UINT32, Error>::Ok(r.Value());
}

/// @brief Send an HTTP GET request to the server
/// @return Ok on success, or Err with Http_SendGetFailed on failure

Result<VOID, Error> HttpClient::SendGetRequest(PCCHAR host, PCCHAR path)
{
	// Build GET request: "GET <path> HTTP/1.1\r\nHost: <host>\r\nConnection: close\r\n\r\n"
	CHAR request[2048];
	Span<CHAR> requestSpan(request, 2000);
	USIZE pos = 0;

	pos = AppendStr(requestSpan, pos, "GET ");
	pos = AppendStr(requestSpan, pos, path);
	pos = AppendStr(requestSpan, pos, " HTTP/1.1\r\nHost: ");
	pos = AppendStr(requestSpan, pos, host);
	pos = AppendStr(requestSpan, pos, "\r\nConnection: close\r\n\r\n");

	request[pos] = '\0';

	auto r = Write(Span<const CHAR>(request, (UINT32)pos));
	if (!r)
		return Result<VOID, Error>::Err(r, Error::Http_SendGetFailed);
	if (r.Value() != (UINT32)pos)
		return Result<VOID, Error>::Err(Error::Http_SendGetFailed);
	return Result<VOID, Error>::Ok();
}

/// @brief Send an HTTP POST request to the server
/// @param host Null-terminated hostname for the Host header
/// @param path Null-terminated request-URI path component
/// @param data The data to be sent in the body of the POST request
/// @return Ok on success, or Err with Http_SendPostFailed on failure

Result<VOID, Error> HttpClient::SendPostRequest(PCCHAR host, PCCHAR path, Span<const CHAR> data)
{
	// Build POST request with Content-Length
	CHAR request[2048];
	Span<CHAR> requestSpan(request, 1900);
	USIZE pos = 0;

	pos = AppendStr(requestSpan, pos, "POST ");
	pos = AppendStr(requestSpan, pos, path);
	pos = AppendStr(requestSpan, pos, " HTTP/1.1\r\nHost: ");
	pos = AppendStr(requestSpan, pos, host);
	pos = AppendStr(requestSpan, pos, "\r\nContent-Length: ");

	// Convert data.Size() to string
	CHAR lenStr[16];
	StringUtils::UIntToStr((UINT32)data.Size(), Span<CHAR>(lenStr));

	pos = AppendStr(requestSpan, pos, lenStr);
	pos = AppendStr(requestSpan, pos, "\r\nConnection: close\r\n\r\n");

	request[pos] = '\0';

	// Send headers
	auto r = Write(Span<const CHAR>(request, (UINT32)pos));
	if (!r)
		return Result<VOID, Error>::Err(r, Error::Http_SendPostFailed);
	if (r.Value() != (UINT32)pos)
		return Result<VOID, Error>::Err(Error::Http_SendPostFailed);

	// Send body
	if (data.Size() > 0)
	{
		auto bodyResult = Write(data);
		if (!bodyResult)
			return Result<VOID, Error>::Err(bodyResult, Error::Http_SendPostFailed);
		if (bodyResult.Value() != (UINT32)data.Size())
			return Result<VOID, Error>::Err(Error::Http_SendPostFailed);
	}

	return Result<VOID, Error>::Ok();
}

/// @brief Parse a URL into its components (host, path, port, secure) and validate the format
/// @param url The URL to be parsed
/// @param host Reference to array to store the parsed host (RFC 1035: max 253 chars + null)
/// @param path Reference to array to store the parsed path (max 2048 chars)
/// @param port Reference to store the parsed port
/// @param secure Reference to store whether the connection is secure (true) or not (false)
/// @return Ok on success, or Err with Http_ParseUrlFailed on failure

Result<VOID, Error> HttpClient::ParseUrl(Span<const CHAR> url, CHAR (&host)[254], CHAR (&path)[2048], UINT16 &port, BOOL &secure)
{
	CHAR portBuffer[6];

	host[0] = '\0';
	path[0] = '\0';
	port = 0;
	secure = false;

	UINT8 schemeLength = 0;
	if (StringUtils::StartsWith<CHAR>(url, "ws://"))
	{
		secure = false;
		schemeLength = 5; // ws://
	}
	else if (StringUtils::StartsWith<CHAR>(url, "wss://"))
	{
		secure = true;
		schemeLength = 6; // wss://
	}
	else if (StringUtils::StartsWith<CHAR>(url, "http://"))
	{
		secure = false;
		schemeLength = 7; // http://
	}
	else if (StringUtils::StartsWith<CHAR>(url, "https://"))
	{
		secure = true;
		schemeLength = 8; // https://
	}
	else
	{
		return Result<VOID, Error>::Err(Error::Http_ParseUrlFailed);
	}

	PCCHAR pHostStart = url.Data() + schemeLength;
	USIZE hostPartLen = url.Size() - schemeLength;

	SSIZE pathIdx = StringUtils::IndexOfChar(Span<const CHAR>(pHostStart, hostPartLen), '/');
	PCCHAR pathStart = (pathIdx >= 0) ? pHostStart + pathIdx : pHostStart + hostPartLen;

	SSIZE portIdx = StringUtils::IndexOfChar(Span<const CHAR>(pHostStart, (USIZE)(pathStart - pHostStart)), ':');
	PCCHAR portStart = (portIdx >= 0) ? pHostStart + portIdx : nullptr;

	if (portStart == nullptr)
	{
		port = secure ? 443 : 80;

		USIZE hostLen = (USIZE)(pathStart - pHostStart);
		if (hostLen == 0 || hostLen > 253)
			return Result<VOID, Error>::Err(Error::Http_ParseUrlFailed);

		Memory::Copy(host, pHostStart, hostLen);
		host[hostLen] = '\0';
	}
	else
	{
		USIZE hostLen = (USIZE)(portStart - pHostStart);
		if (hostLen == 0 || hostLen > 253)
			return Result<VOID, Error>::Err(Error::Http_ParseUrlFailed);

		Memory::Copy(host, pHostStart, hostLen);
		host[hostLen] = '\0';

		USIZE portLen = (USIZE)(pathStart - (portStart + 1));
		if (portLen == 0 || portLen > 5)
			return Result<VOID, Error>::Err(Error::Http_ParseUrlFailed);

		Memory::Copy(portBuffer, portStart + 1, portLen);
		portBuffer[portLen] = '\0';

		for (USIZE i = 0; i < portLen; i++)
			if (portBuffer[i] < '0' || portBuffer[i] > '9')
				return Result<VOID, Error>::Err(Error::Http_ParseUrlFailed);

		auto pnumResult = StringUtils::ParseInt64(portBuffer);
		if (!pnumResult)
			return Result<VOID, Error>::Err(pnumResult, Error::Http_ParseUrlFailed);
		auto& pnum = pnumResult.Value();
		if (pnum == 0 || pnum > 65535)
			return Result<VOID, Error>::Err(Error::Http_ParseUrlFailed);
		port = (UINT16)pnum;
	}

	// Extract path — compute length from span bounds, not null terminator
	USIZE pathLen = (USIZE)((url.Data() + url.Size()) - pathStart);
	if (pathLen == 0)
	{
		path[0] = '/';
		path[1] = '\0';
	}
	else
	{
		if (pathLen > 2047)
			return Result<VOID, Error>::Err(Error::Http_ParseUrlFailed);
		Memory::Copy(path, pathStart, pathLen);
		path[pathLen] = '\0';
	}

	return Result<VOID, Error>::Ok();
}

/// @brief Read HTTP response headers using a rolling 4-byte window
/// @param client The TLS client to read from
/// @param expectedStatus The expected HTTP status code (e.g., 200)
/// @return Ok(contentLength) on success (-1 if Content-Length absent), or Err on failure

Result<INT64, Error> HttpClient::ReadResponseHeaders(TlsClient &client, UINT16 expectedStatus)
{
	// Compute expected "XYZ " pattern for the rolling window (big-endian byte order)
	UINT32 expectedTail =
		((UINT32)('0' + expectedStatus / 100) << 24) |
		((UINT32)('0' + (expectedStatus / 10) % 10) << 16) |
		((UINT32)('0' + expectedStatus % 10) << 8) |
		0x20;

	UINT32 tail = 0;
	UINT32 bytesConsumed = 0;
	BOOL statusValid = false;
	INT64 contentLength = -1;

	// Content-Length state machine
	auto clHeader = "Content-Length: ";
	UINT32 matchIndex = 0;
	BOOL parsingValue = false;
	BOOL atLineStart = true;

	for (;;)
	{
		CHAR c;
		auto readResult = client.Read(Span<CHAR>(&c, 1));
		if (!readResult)
			return Result<INT64, Error>::Err(readResult, Error::Http_ReadHeadersFailed_Read);
		if (readResult.Value() <= 0)
			return Result<INT64, Error>::Err(Error::Http_ReadHeadersFailed_Read);

		tail = (tail << 8) | (UINT8)c;
		bytesConsumed++;

		if (bytesConsumed > 16384)
			return Result<INT64, Error>::Err(Error::Http_ReadHeadersFailed_Read);

		// After 13 bytes, tail holds bytes 9-12: check status code
		if (bytesConsumed == 13)
			statusValid = (tail == expectedTail);

		// Content-Length extraction state machine
		if (parsingValue)
		{
			if (c >= '0' && c <= '9')
			{
				if (contentLength > (INT64)0xFFFFFFFFFFFFF / 10)
					parsingValue = false;
				else
					contentLength = contentLength * 10 + (c - '0');
			}
			else
				parsingValue = false;
		}
		else if (atLineStart)
		{
			matchIndex = 0;
			if (c == ((PCCHAR)clHeader)[0])
				matchIndex = 1;
			atLineStart = false;
		}
		else if (matchIndex > 0 && matchIndex < 16)
		{
			if (c == ((PCCHAR)clHeader)[matchIndex])
			{
				matchIndex++;
				if (matchIndex == 16)
				{
					parsingValue = true;
					contentLength = 0;
				}
			}
			else
			{
				matchIndex = 0;
			}
		}

		if (c == '\n')
			atLineStart = true;

		// Check for \r\n\r\n end-of-headers (0x0D0A0D0A)
		if (tail == 0x0D0A0D0A)
			break;
	}

	if (!statusValid)
		return Result<INT64, Error>::Err(Error::Http_ReadHeadersFailed_Status);

	return Result<INT64, Error>::Ok(contentLength);
}
