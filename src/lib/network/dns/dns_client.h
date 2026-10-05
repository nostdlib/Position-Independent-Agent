#pragma once

/**
 * @file dns_client.h
 * @brief DNS-over-HTTPS (DoH) resolver — resolves hostnames to IP addresses via encrypted DNS
 *
 * @details Implements a minimal DNS stub resolver that sends standard DNS wire-format queries
 * (RFC 1035) over HTTPS (RFC 8484) to public recursive resolvers (Cloudflare 1.1.1.1,
 * Google 8.8.8.8). The resolver supports both A (IPv4) and AAAA (IPv6) record types
 * and automatically falls back from IPv6 to IPv4 when AAAA resolution fails.
 *
 * Transport uses TLS 1.3 with HTTP/1.1 POST to the /dns-query endpoint, sending the
 * raw DNS message in the request body with Content-Type: application/dns-message
 * (RFC 8484 Section 4.1). This avoids plaintext DNS and works in environments where
 * UDP port 53 is blocked.
 *
 * Protocol flow:
 *   1. Build a standard DNS query (RFC 1035 Section 4.1)
 *   2. Open a TLS 1.3 connection to the DoH server
 *   3. POST the query as application/dns-message (RFC 8484 Section 4.1)
 *   4. Read the HTTP response and parse the DNS wire-format answer
 *   5. Extract the A or AAAA record from the answer section
 *
 * @see RFC 1035 — Domain Names: Implementation and Specification
 *      https://datatracker.ietf.org/doc/html/rfc1035
 * @see RFC 3596 — DNS Extensions to Support IP Version 6 (AAAA record)
 *      https://datatracker.ietf.org/doc/html/rfc3596
 * @see RFC 8484 — DNS Queries over HTTPS (DoH)
 *      https://datatracker.ietf.org/doc/html/rfc8484
 *
 * @ingroup network
 */

#include "platform/platform.h"

/**
 * @brief DNS resource record types used in queries
 * @details Defines the QTYPE values carried in the question section of a DNS message.
 * Only record types relevant to address resolution and common lookups are included.
 * @see RFC 1035 Section 3.2.2 — TYPE values
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-3.2.2
 * @see RFC 3596 Section 2.1 — AAAA record type
 *      https://datatracker.ietf.org/doc/html/rfc3596#section-2.1
 */
enum class DnsRecordType
{
	A = 1,      ///< IPv4 host address — 4-byte address (RFC 1035 Section 3.4.1)
	AAAA = 28,  ///< IPv6 host address — 16-byte address (RFC 3596 Section 2.1)
	CNAME = 5,  ///< Canonical name — alias for another domain (RFC 1035 Section 3.3.1)
	MX = 15,    ///< Mail exchange — mail routing (RFC 1035 Section 3.3.9)
	NS = 2,     ///< Authoritative name server (RFC 1035 Section 3.3.11)
	PTR = 12,   ///< Domain name pointer — reverse DNS (RFC 1035 Section 3.3.12)
	TXT = 16    ///< Text strings — arbitrary text data (RFC 1035 Section 3.3.14)
};

/**
 * @brief DNS-over-HTTPS stub resolver
 * @details Provides hostname-to-IP resolution by sending RFC 1035 wire-format queries
 * over HTTPS (RFC 8484) to public recursive DNS servers. All queries use HTTP POST
 * with Content-Type: application/dns-message to avoid base64 encoding overhead.
 *
 * The resolver tries multiple DNS servers with automatic fallback:
 *   - Primary: Cloudflare (1.1.1.1, 1.0.0.1)
 *   - Secondary: Google (8.8.8.8, 8.8.4.4)
 *
 * When AAAA (IPv6) resolution fails, the resolver automatically retries with A (IPv4).
 *
 * @see RFC 8484 Section 4.1 — DNS Wire Format for DoH POST requests
 *      https://datatracker.ietf.org/doc/html/rfc8484#section-4.1
 */
class DnsClient
{
private:
	/**
	 * @brief Resolves a hostname via DNS-over-HTTPS to a single DoH server
	 * @param host Hostname to resolve (e.g., "example.com")
	 * @param dnsServerIp IP address of the DoH server to query
	 * @param dnsServerName TLS SNI hostname for the DoH server (e.g., "one.one.one.one")
	 * @param dnstype Record type to query — A (IPv4) or AAAA (IPv6), defaults to AAAA
	 * @return Ok(IPAddress) on successful resolution, or Err on connection/query/parse failure
	 *
	 * @details Opens a TLS 1.3 connection to the DoH server on port 443, constructs a
	 * standard DNS query in wire format (RFC 1035 Section 4.1), and sends it as an
	 * HTTP/1.1 POST request to /dns-query with Content-Type: application/dns-message
	 * (RFC 8484 Section 4.1). Parses the HTTP response body as a DNS wire-format message
	 * and extracts the first A or AAAA record from the answer section.
	 *
	 * Short-circuits for "localhost" without network access.
	 *
	 * @see RFC 8484 Section 4.1 — DNS Wire Format (POST method)
	 *      https://datatracker.ietf.org/doc/html/rfc8484#section-4.1
	 * @see RFC 1035 Section 4.1 — Message Format
	 *      https://datatracker.ietf.org/doc/html/rfc1035#section-4.1
	 */
	[[nodiscard]] static Result<IPAddress, Error> ResolveOverHttp(Span<const CHAR> host, const IPAddress &dnsServerIp, Span<const CHAR> dnsServerName, DnsRecordType dnstype = DnsRecordType::AAAA);

	/**
	 * @brief Tries multiple IP addresses for a single DoH provider until one succeeds
	 * @param host Hostname to resolve
	 * @param ips Span of DoH server IP addresses to try in order
	 * @param serverName TLS SNI hostname for the DoH server
	 * @param dnstype Record type to query — A (IPv4) or AAAA (IPv6)
	 * @return Ok(IPAddress) from the first successful server, or Err(Dns_ResolveFailed) if all fail
	 *
	 * @details Iterates through the server IP array, calling ResolveOverHttp for each.
	 * Returns immediately on the first successful resolution. This provides resilience
	 * against individual server failures (e.g., Cloudflare 1.1.1.1 down, fallback to 1.0.0.1).
	 */
	template <USIZE N>
	[[nodiscard]] static Result<IPAddress, Error> ResolveWithFallback(Span<const CHAR> host, Span<const IPAddress, N> ips, Span<const CHAR> serverName, DnsRecordType dnstype)
	{
		for (USIZE i = 0; i < N; i++)
		{
			auto result = ResolveOverHttp(host, ips[i], serverName, dnstype);
			if (result)
				return result;
		}
		return Result<IPAddress, Error>::Err(Error::Dns_ResolveFailed);
	}

public:
	/**
	 * @brief Resolves a hostname to an IP address using DoH with automatic provider and protocol fallback
	 * @param host Hostname to resolve (e.g., "example.com")
	 * @param dnstype Record type to query — A (IPv4) or AAAA (IPv6), defaults to AAAA
	 * @return Ok(IPAddress) on success, or Err(Dns_ResolveFailed) if all providers and fallbacks fail
	 *
	 * @details Primary entry point for DNS resolution. Tries providers in order:
	 *   1. Cloudflare DoH (1.1.1.1, 1.0.0.1)
	 *   2. Google DoH (8.8.8.8, 8.8.4.4)
	 *
	 * Short-circuits without network I/O when the host is already an IP literal
	 * (IPv4 or IPv6) or "localhost".
	 *
	 * If the requested type is AAAA and all attempts fail, automatically retries with A (IPv4)
	 * through both providers. This handles environments without IPv6 connectivity.
	 */
	[[nodiscard]] static Result<IPAddress, Error> Resolve(Span<const CHAR> host, DnsRecordType dnstype = DnsRecordType::AAAA);

	/**
	 * @brief Resolves a hostname via Cloudflare DNS-over-HTTPS (1.1.1.1 / 1.0.0.1)
	 * @param host Hostname to resolve
	 * @param dnstype Record type to query — A (IPv4) or AAAA (IPv6), defaults to AAAA
	 * @return Ok(IPAddress) on success, or Err if both Cloudflare servers fail
	 *
	 * @details Queries Cloudflare's public DoH service at https://one.one.one.one/dns-query.
	 * Tries the primary server (1.1.1.1) first, then falls back to the secondary (1.0.0.1).
	 *
	 * @see https://developers.cloudflare.com/1.1.1.1/encryption/dns-over-https/
	 */
	[[nodiscard]] static Result<IPAddress, Error> CloudflareResolve(Span<const CHAR> host, DnsRecordType dnstype = DnsRecordType::AAAA);

	/**
	 * @brief Resolves a hostname via Google DNS-over-HTTPS (8.8.8.8 / 8.8.4.4)
	 * @param host Hostname to resolve
	 * @param dnstype Record type to query — A (IPv4) or AAAA (IPv6), defaults to AAAA
	 * @return Ok(IPAddress) on success, or Err if both Google servers fail
	 *
	 * @details Queries Google's public DoH service at https://dns.google/dns-query.
	 * Tries the primary server (8.8.8.8) first, then falls back to the secondary (8.8.4.4).
	 *
	 * @see https://developers.google.com/speed/public-dns/docs/doh
	 */
	[[nodiscard]] static Result<IPAddress, Error> GoogleResolve(Span<const CHAR> host, DnsRecordType dnstype = DnsRecordType::AAAA);
};
