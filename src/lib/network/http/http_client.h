#pragma once

/**
 * @file http.h
 * @brief HTTP/1.1 client with TLS 1.3 support for HTTPS connections
 *
 * @details Implements a minimal HTTP/1.1 client that supports both plaintext HTTP
 * and encrypted HTTPS (via TLS 1.3). The client resolves hostnames using DNS-over-HTTPS,
 * parses URLs into host/path/port components, and performs GET and POST requests.
 *
 * Response parsing uses a rolling-window approach to read HTTP headers without
 * requiring a large buffer, extracting the status code and Content-Length header.
 *
 * @see RFC 9110 — HTTP Semantics
 *      https://datatracker.ietf.org/doc/html/rfc9110
 * @see RFC 9112 — HTTP/1.1
 *      https://datatracker.ietf.org/doc/html/rfc9112
 * @see RFC 2818 — HTTP Over TLS
 *      https://datatracker.ietf.org/doc/html/rfc2818
 */

#include "platform/platform.h"
#include "lib/network/tls/tls_client.h"

/// HTTP client for making HTTP/1.1 requests over plaintext or TLS 1.3 connections
class HttpClient
{
private:
	IPAddress ipAddress;
	UINT16 port;
	TlsClient tlsContext;

	// Private constructor — only used by Create()
	HttpClient(const IPAddress &ip, UINT16 portNum, TlsClient &&tls)
		: ipAddress(ip), port(portNum), tlsContext(static_cast<TlsClient &&>(tls))
	{
	}

public:
	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	// Placement new required by Result<HttpClient, Error>
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}

	~HttpClient()
	{
		if (IsValid())
			(VOID)Close();
	}

	HttpClient(const HttpClient &) = delete;
	HttpClient &operator=(const HttpClient &) = delete;

	HttpClient(HttpClient &&other) noexcept
		: ipAddress(other.ipAddress), port(other.port),
		  tlsContext(static_cast<TlsClient &&>(other.tlsContext))
	{
		other.port = 0;
	}

	HttpClient &operator=(HttpClient &&other) noexcept
	{
		if (this != &other)
		{
			if (IsValid())
				(VOID)Close();
			ipAddress = other.ipAddress;
			port = other.port;
			tlsContext = static_cast<TlsClient &&>(other.tlsContext);
			other.port = 0;
		}
		return *this;
	}

	/**
	 * @brief Create an HttpClient from a URL string
	 *
	 * @details Parses the URL into scheme, host, port, and path components, resolves the
	 * hostname via DNS-over-HTTPS, and constructs a TLS client for the resolved address.
	 * Falls back to IPv4 if the initial IPv6 connection attempt fails (e.g., on UEFI).
	 *
	 * @param url The URL to connect to (e.g., "https://example.com/path")
	 * @return The constructed HttpClient, or an error if URL parsing or DNS resolution fails
	 *
	 * @see RFC 3986 — Uniform Resource Identifier (URI): Generic Syntax
	 *      https://datatracker.ietf.org/doc/html/rfc3986
	 */
	[[nodiscard]] static Result<HttpClient, Error> Create(Span<const CHAR> url);

	constexpr BOOL IsValid() const { return tlsContext.IsValid(); }
	constexpr BOOL IsSecure() const { return tlsContext.IsSecure(); }

	/**
	 * @brief Open the connection to the remote server
	 *
	 * @details Establishes the underlying TCP connection and, for HTTPS URLs,
	 * performs the TLS 1.3 handshake.
	 *
	 * @return Ok on success, or an error if the connection or TLS handshake fails
	 *
	 * @see RFC 9112 Section 3 — Connection Management
	 *      https://datatracker.ietf.org/doc/html/rfc9112#section-3
	 */
	[[nodiscard]] Result<VOID, Error> Open();
	/**
	 * @brief Close the connection and release resources
	 *
	 * @details Sends a TLS close_notify alert (for HTTPS) and closes the underlying
	 * TCP socket.
	 *
	 * @return Ok on success, or an error if the shutdown fails
	 *
	 * @see RFC 9112 Section 3 — Connection Management
	 *      https://datatracker.ietf.org/doc/html/rfc9112#section-3
	 */
	[[nodiscard]] Result<VOID, Error> Close();
	/**
	 * @brief Read data from the connection into the provided buffer
	 *
	 * @details Reads plaintext data from the connection, decrypting if HTTPS.
	 *
	 * @param buffer The buffer to read into
	 * @return The number of bytes read, or an error
	 */
	[[nodiscard]] Result<SSIZE, Error> Read(Span<CHAR> buffer);
	/**
	 * @brief Write data to the connection
	 *
	 * @details Writes data to the connection, encrypting if HTTPS.
	 *
	 * @param buffer The data to write
	 * @return The number of bytes written, or an error
	 */
	[[nodiscard]] Result<UINT32, Error> Write(Span<const CHAR> buffer);

	/**
	 * @brief Send an HTTP GET request with the given host and path
	 *
	 * @details Constructs and sends an HTTP/1.1 GET request with Host and
	 * Connection: close headers. The request line follows the format defined
	 * in RFC 9112 Section 3.
	 *
	 * @param host Null-terminated hostname for the Host header
	 * @param path Null-terminated request-URI path component
	 * @return Ok on success, or an error if the write fails
	 *
	 * @see RFC 9110 Section 9.3.1 — GET
	 *      https://datatracker.ietf.org/doc/html/rfc9110#section-9.3.1
	 * @see RFC 9112 Section 3 — Request Line
	 *      https://datatracker.ietf.org/doc/html/rfc9112#section-3
	 */
	[[nodiscard]] Result<VOID, Error> SendGetRequest(PCCHAR host, PCCHAR path);
	/**
	 * @brief Send an HTTP POST request with the given body data
	 *
	 * @details Constructs and sends an HTTP/1.1 POST request with Host,
	 * Content-Length, and Connection: close headers, followed by the request body.
	 *
	 * @param host Null-terminated hostname for the Host header
	 * @param path Null-terminated request-URI path component
	 * @param data The request body to send
	 * @return Ok on success, or an error if the write fails
	 *
	 * @see RFC 9110 Section 9.3.3 — POST
	 *      https://datatracker.ietf.org/doc/html/rfc9110#section-9.3.3
	 * @see RFC 9112 Section 6 — Message Body
	 *      https://datatracker.ietf.org/doc/html/rfc9112#section-6
	 */
	[[nodiscard]] Result<VOID, Error> SendPostRequest(PCCHAR host, PCCHAR path, Span<const CHAR> data);
	/**
	 * @brief Parse a URL into its components and validate the format
	 *
	 * @details Extracts scheme (http/https/ws/wss), hostname (max 253 chars per
	 * RFC 1035), optional port, and path from the URL. Defaults to port 80 for
	 * plaintext and 443 for secure schemes when no port is specified.
	 *
	 * @param url The URL string to parse
	 * @param host Output buffer for the hostname (max 253 chars + null)
	 * @param path Output buffer for the path component
	 * @param port Output for the port number (defaults to 80 or 443)
	 * @param secure Output flag indicating HTTPS (true) or HTTP (false)
	 * @return Ok on success, or an error if the URL format is invalid
	 *
	 * @see RFC 3986 — Uniform Resource Identifier (URI): Generic Syntax
	 *      https://datatracker.ietf.org/doc/html/rfc3986
	 * @see RFC 3986 Section 3 — Syntax Components
	 *      https://datatracker.ietf.org/doc/html/rfc3986#section-3
	 */
	[[nodiscard]] static Result<VOID, Error> ParseUrl(Span<const CHAR> url, CHAR (&host)[254], CHAR (&path)[2048], UINT16 &port, BOOL &secure);
	/**
	 * @brief Read HTTP response headers using a rolling window
	 *
	 * @details Reads the HTTP response one byte at a time using a 4-byte rolling
	 * window to detect the end-of-headers marker (CRLFCRLF). Validates the status
	 * code at byte offset 9-12 and extracts the Content-Length header value using
	 * a character-by-character state machine. Aborts if headers exceed 16 KiB.
	 *
	 * @param client The TLS client to read from
	 * @param expectedStatus The expected HTTP status code (e.g., 200)
	 * @return The Content-Length value (-1 if not present), or an error if status mismatches
	 *
	 * @see RFC 9112 Section 4 — Status Line
	 *      https://datatracker.ietf.org/doc/html/rfc9112#section-4
	 * @see RFC 9110 Section 8.6 — Content-Length
	 *      https://datatracker.ietf.org/doc/html/rfc9110#section-8.6
	 */
	[[nodiscard]] static Result<INT64, Error> ReadResponseHeaders(TlsClient &client, UINT16 expectedStatus);
};
