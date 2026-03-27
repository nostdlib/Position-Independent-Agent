#include "lib/network/dns/dns_client.h"
#include "core/binary/binary_reader.h"
#include "lib/network/http/http_client.h"
#include "platform/console/logger.h"
#include "core/memory/memory.h"
#include "core/string/string.h"
#include "lib/network/tls/tls_client.h"

/**
 * @brief DNS message header — fixed 12-byte structure at the start of every DNS message
 * @details Maps directly to the wire format defined in RFC 1035 Section 4.1.1.
 * All multi-byte fields are in network byte order (big-endian).
 *
 * Wire layout (96 bits / 12 bytes):
 * @code
 *   0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                      ID                         |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |QR|   Opcode  |AA|TC|RD|RA| Z|AD|CD|   RCODE    |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                    QDCOUNT                       |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                    ANCOUNT                       |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                    NSCOUNT                       |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                    ARCOUNT                       |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 * @endcode
 *
 * @note The Z field is shown as 3 bits in RFC 1035, but RFC 2535 and RFC 4035 split it
 * into Z (1 bit, must be zero), AD (Authentic Data, 1 bit), and CD (Checking Disabled, 1 bit).
 *
 * @see RFC 1035 Section 4.1.1 — Header section format
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-4.1.1
 * @see RFC 4035 Section 3.2.2 — AD and CD bit definitions
 *      https://datatracker.ietf.org/doc/html/rfc4035#section-3.2.2
 */
typedef struct _DNS_REQUEST_HEADER
{
	UINT16 Id;        ///< Transaction ID — copied from query to response for matching
	UCHAR Rd : 1;     ///< Recursion Desired — set to 1 to request recursive resolution
	UCHAR Tc : 1;     ///< TrunCation — set if the message was truncated (>512 bytes over UDP)
	UCHAR Aa : 1;     ///< Authoritative Answer — set if the responding server is authoritative
	UCHAR Opcode : 4; ///< Operation code — 0=QUERY, 1=IQUERY, 2=STATUS (RFC 1035 Section 4.1.1)
	UCHAR Qr : 1;     ///< Query/Response — 0=query, 1=response

	UCHAR Rcode : 4;  ///< Response code — 0=NoError, 1=FormErr, 2=ServFail, 3=NXDomain (RFC 1035 Section 4.1.1)
	UCHAR Cd : 1;     ///< Checking Disabled — disables DNSSEC validation (RFC 4035)
	UCHAR Ad : 1;     ///< Authenticated Data — indicates DNSSEC-validated response (RFC 4035)
	UCHAR Z : 1;      ///< Reserved — must be zero in all queries and responses (RFC 1035)
	UCHAR Ra : 1;     ///< Recursion Available — set by server if it supports recursion

	UINT16 QdCount;   ///< QDCOUNT — number of entries in the question section
	UINT16 AnsCount;  ///< ANCOUNT — number of resource records in the answer section
	UINT16 AuthCount; ///< NSCOUNT — number of name server resource records in the authority section
	UINT16 AddCount;  ///< ARCOUNT — number of resource records in the additional records section
} DNS_REQUEST_HEADER, *PDNS_REQUEST_HEADER;

/**
 * @brief DNS question entry — follows the QNAME in the question section
 * @details Each question entry consists of a variable-length QNAME (encoded as
 * length-prefixed labels) followed by this fixed 4-byte structure containing
 * the query type and class.
 *
 * @see RFC 1035 Section 4.1.2 — Question section format
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-4.1.2
 */
typedef struct _DNS_REQUEST_QUESTION
{
	UINT16 QType;  ///< QTYPE — type of the query (e.g., A=1, AAAA=28, CNAME=5)
	UINT16 QClass; ///< QCLASS — class of the query (IN=1 for Internet)
} DNS_REQUEST_QUESTION, *PDNS_REQUEST_QUESTION;

static_assert(sizeof(DNS_REQUEST_HEADER) == 12, "DNS header must be 12 bytes (no padding)");
static_assert(sizeof(DNS_REQUEST_QUESTION) == 4, "DNS question must be 4 bytes (no padding)");

/**
 * @brief Skips over a DNS domain name in wire format (labels or compressed pointer)
 * @param data Span of bytes starting at the name to skip
 * @return Ok(bytes consumed) on success, or Err(Dns_ParseFailed) on malformed input
 *
 * @details DNS names in wire format are encoded as a sequence of length-prefixed labels
 * terminated by a zero-length label (root), or compressed using a 2-byte pointer.
 *
 * Label encoding (RFC 1035 Section 4.1.2):
 *   - Each label starts with a 1-byte length (0–63), followed by that many octets
 *   - The name ends with a zero-length label (0x00)
 *
 * Name compression (RFC 1035 Section 4.1.4):
 *   - A pointer is identified by the two high bits being set (0xC0)
 *   - The pointer is a 2-byte value; the remaining 14 bits are an offset into the message
 *   - Pointers allow names to reference earlier occurrences, reducing message size
 *
 * This function does not follow compression pointers — it only determines how many
 * bytes the name occupies at the current position for the purpose of advancing past it.
 *
 * @see RFC 1035 Section 4.1.2 — Question section format (QNAME encoding)
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-4.1.2
 * @see RFC 1035 Section 4.1.4 — Message compression
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-4.1.4
 */
[[nodiscard]] static Result<INT32, Error> SkipName(Span<const UINT8> data)
{
	INT32 offset = 0;
	while (offset < (INT32)data.Size())
	{
		UINT8 label = data[offset];

		if (label == 0)
			return Result<INT32, Error>::Ok(offset + 1);

		if (label >= 0xC0)
		{
			if (offset + 2 > (INT32)data.Size())
				return Result<INT32, Error>::Err(Error::Dns_ParseFailed);
			return Result<INT32, Error>::Ok(offset + 2);
		}

		if (label > 63)
		{
			LOG_WARNING("SkipName: invalid label length: %d", label);
			return Result<INT32, Error>::Err(Error::Dns_ParseFailed);
		}

		offset += label + 1;
	}
	return Result<INT32, Error>::Err(Error::Dns_ParseFailed);
}

/**
 * @brief Parses the answer section of a DNS response and extracts the first A or AAAA record
 * @param data Span of bytes covering the answer section (starting after the question section)
 * @param answerCount Number of answer resource records (ANCOUNT from the header)
 * @return Ok(IPAddress) with the resolved IP address, or Err(Dns_ParseFailed) if no matching record
 *
 * @details Iterates through the answer section resource records (RRs) as defined in
 * RFC 1035 Section 4.1.3. Each RR has the format:
 *
 * @code
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *   |                      NAME                        |  (variable, compressed or labels)
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *   |                      TYPE                        |  (2 bytes — A=1, AAAA=28)
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *   |                     CLASS                        |  (2 bytes — IN=1)
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *   |                      TTL                         |  (4 bytes — time to live)
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *   |                   RDLENGTH                       |  (2 bytes — length of RDATA)
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *   |                     RDATA                        |  (RDLENGTH bytes)
 *   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 * @endcode
 *
 * For A records (TYPE=1), RDATA is a 4-byte IPv4 address (RFC 1035 Section 3.4.1).
 * For AAAA records (TYPE=28), RDATA is a 16-byte IPv6 address (RFC 3596 Section 2.2).
 * CNAME and other record types are skipped.
 *
 * @see RFC 1035 Section 4.1.3 — Resource record format
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-4.1.3
 * @see RFC 1035 Section 3.4.1 — A RDATA format (IPv4)
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-3.4.1
 * @see RFC 3596 Section 2.2 — AAAA RDATA format (IPv6)
 *      https://datatracker.ietf.org/doc/html/rfc3596#section-2.2
 */
[[nodiscard]] static Result<IPAddress, Error> ParseAnswer(Span<const UINT8> data, INT32 answerCount)
{
	// type(2) + class(2) + ttl(4) + rdlength(2)
	constexpr INT32 FIXED_FIELDS_SIZE = 10;

	BinaryReader reader(data);

	while (answerCount > 0)
	{
		if (reader.Remaining() == 0)
			break;

		auto skipResult = SkipName(Span<const UINT8>((const UINT8 *)reader.Current(), reader.Remaining()));
		if (!skipResult)
		{
			LOG_WARNING("ParseAnswer: failed to skip answer name");
			break;
		}
		reader.Skip((USIZE)skipResult.Value());

		if ((INT32)reader.Remaining() < FIXED_FIELDS_SIZE)
		{
			LOG_WARNING("ParseAnswer: truncated fixed fields");
			break;
		}

		UINT16 type = reader.ReadU16BE();
		reader.Skip(2); // class
		reader.Skip(4); // ttl
		UINT16 rdlength = reader.ReadU16BE();

		if ((INT32)reader.Remaining() < rdlength)
		{
			LOG_WARNING("ParseAnswer: truncated rdata");
			break;
		}

		if (type == static_cast<UINT16>(DnsRecordType::A) && rdlength == 4)
		{
			UINT32 ipv4;
			Memory::Copy(&ipv4, reader.Current(), 4);
			return Result<IPAddress, Error>::Ok(IPAddress::FromIPv4(ipv4));
		}
		else if (type == static_cast<UINT16>(DnsRecordType::AAAA) && rdlength == 16)
		{
			UINT8 ipv6Bytes[16];
			Memory::Copy(ipv6Bytes, reader.Current(), 16);
			return Result<IPAddress, Error>::Ok(IPAddress::FromIPv6(ipv6Bytes));
		}

		reader.Skip(rdlength);
		answerCount--;
	}

	return Result<IPAddress, Error>::Err(Error::Dns_ParseFailed);
}

/**
 * @brief Parses the question section of a DNS response, advancing past all question entries
 * @param data Span of bytes starting at the question section
 * @param questionCount Number of question entries to parse (QDCOUNT from the header)
 * @return Ok(total bytes consumed) on success, or Err(Dns_ParseFailed) on malformed input
 *
 * @details Each question entry consists of a variable-length QNAME followed by a fixed
 * 4-byte QTYPE+QCLASS structure (RFC 1035 Section 4.1.2). This function skips over
 * all question entries to position the reader at the start of the answer section.
 *
 * @see RFC 1035 Section 4.1.2 — Question section format
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-4.1.2
 */
[[nodiscard]] static Result<INT32, Error> ParseQuery(Span<const UINT8> data, INT32 questionCount)
{
	BinaryReader reader(data);

	while (questionCount > 0)
	{
		if (reader.Remaining() == 0)
		{
			LOG_WARNING("ParseQuery: buffer exhausted");
			return Result<INT32, Error>::Err(Error::Dns_ParseFailed);
		}

		auto skipResult = SkipName(Span<const UINT8>((const UINT8 *)reader.Current(), reader.Remaining()));
		if (!skipResult)
		{
			LOG_WARNING("ParseQuery: invalid name length");
			return Result<INT32, Error>::Err(skipResult, Error::Dns_ParseFailed);
		}

		INT32 entrySize = skipResult.Value() + (INT32)sizeof(DNS_REQUEST_QUESTION);
		if ((INT32)reader.Remaining() < entrySize)
		{
			LOG_WARNING("ParseQuery: truncated question entry");
			return Result<INT32, Error>::Err(Error::Dns_ParseFailed);
		}

		reader.Skip((USIZE)entrySize);
		questionCount--;
	}
	return Result<INT32, Error>::Ok((INT32)reader.GetOffset());
}

/**
 * @brief Parses a complete DNS response message and extracts the resolved IP address
 * @param data Span of bytes containing the full DNS response (header + question + answer sections)
 * @return Ok(IPAddress) on success, or Err(Dns_ParseFailed) on any parse error or missing answer
 *
 * @details Validates and parses a DNS response message per RFC 1035 Section 4.1:
 *   1. Reads the 12-byte header and verifies QR=1 (response) and RCODE=0 (no error)
 *   2. Validates ANCOUNT (1–20) and QDCOUNT (0–10) for sanity
 *   3. Skips the question section by parsing each QNAME + QTYPE/QCLASS entry
 *   4. Delegates to ParseAnswer() to extract the IP from the answer section
 *
 * @see RFC 1035 Section 4.1 — Format (header, question, answer, authority, additional)
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-4.1
 * @see RFC 1035 Section 4.1.1 — Header section format (QR, RCODE fields)
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-4.1.1
 */
[[nodiscard]] static Result<IPAddress, Error> ParseDnsResponse(Span<const UINT8> data)
{
	if ((INT32)data.Size() < (INT32)sizeof(DNS_REQUEST_HEADER))
	{
		LOG_WARNING("ParseDnsResponse: invalid parameters");
		return Result<IPAddress, Error>::Err(Error::Dns_ParseFailed);
	}

	BinaryReader reader(data);

	// Skip 2-byte transaction ID (RFC 1035 Section 4.1.1)
	reader.Skip(2);

	// Read flags word — QR bit (0x8000) must be set for a response
	UINT16 flags = reader.ReadU16BE();
	if (!(flags & 0x8000))
	{
		LOG_WARNING("ParseDnsResponse: not a response");
		return Result<IPAddress, Error>::Err(Error::Dns_ParseFailed);
	}

	// RCODE (low 4 bits) — 0 means no error (RFC 1035 Section 4.1.1)
	UINT8 rcode = flags & 0x000F;
	if (rcode != 0)
	{
		LOG_WARNING("ParseDnsResponse: server returned error (rcode=%d)", rcode);
		return Result<IPAddress, Error>::Err(Error::Dns_ParseFailed);
	}

	UINT16 qCount = reader.ReadU16BE();
	UINT16 ansCount = reader.ReadU16BE();

	if (ansCount == 0 || ansCount > 20)
	{
		LOG_WARNING("ParseDnsResponse: invalid answer count: %d", ansCount);
		return Result<IPAddress, Error>::Err(Error::Dns_ParseFailed);
	}

	if (qCount > 10)
	{
		LOG_WARNING("ParseDnsResponse: suspicious question count: %d", qCount);
		return Result<IPAddress, Error>::Err(Error::Dns_ParseFailed);
	}

	// Skip NSCOUNT and ARCOUNT (4 bytes total)
	reader.Skip(4);

	if (qCount > 0)
	{
		auto queryResult = ParseQuery(Span<const UINT8>((const UINT8 *)reader.Current(), reader.Remaining()), qCount);
		if (!queryResult)
		{
			LOG_WARNING("ParseDnsResponse: invalid query section");
			return Result<IPAddress, Error>::Err(queryResult, Error::Dns_ParseFailed);
		}
		reader.Skip((USIZE)queryResult.Value());
	}

	if (reader.Remaining() == 0)
	{
		LOG_WARNING("ParseDnsResponse: no space for answer section");
		return Result<IPAddress, Error>::Err(Error::Dns_ParseFailed);
	}

	return ParseAnswer(Span<const UINT8>((const UINT8 *)reader.Current(), reader.Remaining()), ansCount);
}

/**
 * @brief Encodes a hostname into DNS wire format (length-prefixed labels)
 * @param output Span of bytes to write the encoded name into
 * @param host Hostname (e.g., "www.example.com")
 * @return Ok(bytes written) including the terminating zero label, or Err(Dns_QueryFailed) on error
 *
 * @details Converts a dot-delimited hostname into DNS wire format as defined in
 * RFC 1035 Section 4.1.2. Each label is preceded by a 1-byte length, and the
 * name is terminated by a zero-length label (0x00).
 *
 * Example: "www.example.com" encodes as:
 * @code
 *   03 77 77 77  07 65 78 61 6D 70 6C 65  03 63 6F 6D  00
 *   ^3 w  w  w   ^7 e  x  a  m  p  l  e   ^3 c  o  m   ^0 (root)
 * @endcode
 *
 * Validation per RFC 1035 Section 2.3.1:
 *   - Each label must be 1–63 octets (empty labels / consecutive dots are rejected)
 *   - Total encoded name must fit in the output buffer
 *
 * @see RFC 1035 Section 4.1.2 — QNAME encoding
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-4.1.2
 * @see RFC 1035 Section 2.3.1 — Preferred name syntax
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-2.3.1
 */
[[nodiscard]] static Result<INT32, Error> FormatDnsName(Span<UINT8> output, Span<const CHAR> host)
{
	if (host.Size() == 0 || output.Size() == 0)
		return Result<INT32, Error>::Err(Error::Dns_QueryFailed);

	// Worst case: host.Size() bytes + 1 extra label-length byte + null terminator
	if ((INT32)(host.Size() + 2) > (INT32)output.Size())
		return Result<INT32, Error>::Err(Error::Dns_QueryFailed);

	INT32 written = 0;
	UINT32 i, t = 0;
	for (i = 0; i < (UINT32)host.Size(); i++)
	{
		if (host[i] == '.')
		{
			UINT32 labelLen = i - t;
			if (labelLen == 0)
				return Result<INT32, Error>::Err(Error::Dns_QueryFailed); // empty label (leading dot or consecutive dots)
			if (labelLen > 63)
				return Result<INT32, Error>::Err(Error::Dns_QueryFailed); // RFC 1035: label max 63 octets
			if (written + 1 + (INT32)labelLen >= (INT32)output.Size())
				return Result<INT32, Error>::Err(Error::Dns_QueryFailed);
			output[written++] = (UINT8)labelLen;
			for (; t < i; t++)
				output[written++] = host[t];
			t++;
		}
	}
	if (host.Size() > 0 && host[(UINT32)host.Size() - 1] != '.')
	{
		UINT32 labelLen = i - t;
		if (labelLen == 0)
			return Result<INT32, Error>::Err(Error::Dns_QueryFailed); // trailing empty segment (shouldn't happen, but guard)
		if (labelLen > 63)
			return Result<INT32, Error>::Err(Error::Dns_QueryFailed); // RFC 1035: label max 63 octets
		if (written + 1 + (INT32)labelLen >= (INT32)output.Size())
			return Result<INT32, Error>::Err(Error::Dns_QueryFailed);
		output[written++] = (UINT8)labelLen;
		for (; t < i; t++)
			output[written++] = host[t];
	}
	output[written++] = '\0';
	return Result<INT32, Error>::Ok(written);
}

/**
 * @brief Constructs a DNS query message in wire format for use with DoH (RFC 8484)
 * @param host Hostname to query
 * @param dnstype Record type — A (1) for IPv4 or AAAA (28) for IPv6
 * @param buffer Output byte span to write the DNS query message into
 * @return Ok(query size in bytes) on success, or Err(Dns_QueryFailed) if the buffer is too small
 *
 * @details Builds a complete DNS query message per RFC 1035 Section 4.1, consisting of:
 *   1. A 12-byte header with QR=0 (query), RD=1 (recursion desired), QDCOUNT=1
 *   2. A question section with the hostname encoded as a QNAME (length-prefixed labels),
 *      followed by QTYPE and QCLASS=IN(1)
 *
 * The query uses a fixed transaction ID (0x24a1) since DoH uses HTTP request/response
 * pairing for correlation rather than the DNS ID field (RFC 8484 Section 4.1).
 *
 * No TCP length prefix is included — the query is sent as the raw HTTP POST body
 * with Content-Type: application/dns-message per RFC 8484.
 *
 * @see RFC 1035 Section 4.1 — DNS message format
 *      https://datatracker.ietf.org/doc/html/rfc1035#section-4.1
 * @see RFC 8484 Section 4.1 — DNS Wire Format (POST method, no TCP length prefix)
 *      https://datatracker.ietf.org/doc/html/rfc8484#section-4.1
 */
[[nodiscard]] static Result<UINT32, Error> GenerateQuery(Span<const CHAR> host, DnsRecordType dnstype, Span<UINT8> buffer)
{
	if (buffer.Size() < sizeof(DNS_REQUEST_HEADER) + sizeof(DNS_REQUEST_QUESTION) + 2)
		return Result<UINT32, Error>::Err(Error::Dns_QueryFailed);

	PDNS_REQUEST_HEADER header = (PDNS_REQUEST_HEADER)buffer.Data();

	header->Id = (UINT16)0x24a1;
	header->Qr = 0;
	header->Opcode = 0;
	header->Aa = 0;
	header->Tc = 0;
	header->Rd = 1;
	header->Ra = 0;
	header->Z = 0;
	header->Ad = 0;
	header->Cd = 0;
	header->Rcode = 0;
	header->QdCount = ByteOrder::Swap16(1);
	header->AnsCount = 0;
	header->AuthCount = 0;
	header->AddCount = 0;

	UINT8 *qname = buffer.Data() + sizeof(DNS_REQUEST_HEADER);
	INT32 nameSpaceLeft = (INT32)(buffer.Size() - sizeof(DNS_REQUEST_HEADER) - sizeof(DNS_REQUEST_QUESTION));
	auto nameResult = FormatDnsName(Span<UINT8>(qname, (USIZE)nameSpaceLeft), host);
	if (!nameResult)
	{
		LOG_WARNING("GenerateQuery: hostname too long for buffer");
		return Result<UINT32, Error>::Err(nameResult, Error::Dns_QueryFailed);
	}
	INT32 nameLen = nameResult.Value();

	PDNS_REQUEST_QUESTION question = (PDNS_REQUEST_QUESTION)(qname + nameLen);
	question->QClass = ByteOrder::Swap16(1);
	question->QType = ByteOrder::Swap16(static_cast<UINT16>(dnstype));

	return Result<UINT32, Error>::Ok((UINT32)(sizeof(DNS_REQUEST_HEADER) + nameLen + sizeof(DNS_REQUEST_QUESTION)));
}

/**
 * @brief Resolves a hostname via DNS-over-HTTPS (DoH) to a single DoH server
 * @param host Hostname to resolve
 * @param dnsServerIp IP address of the DoH server
 * @param dnsServerName TLS SNI hostname for certificate validation
 * @param dnstype Record type — A (IPv4) or AAAA (IPv6)
 * @return Ok(IPAddress) on success, or Err with a DNS error code on failure
 *
 * @details Implements the DNS-over-HTTPS protocol (RFC 8484) using the POST method:
 *   1. Short-circuits for "localhost" without any network I/O
 *   2. Establishes a TLS 1.3 connection to the DoH server on port 443
 *   3. Constructs a DNS wire-format query via GenerateQuery()
 *   4. Sends an HTTP/1.1 POST request:
 *      - Path: /dns-query
 *      - Content-Type: application/dns-message
 *      - Accept: application/dns-message
 *      - Body: raw DNS query bytes
 *   5. Reads the HTTP response, validates status 200
 *   6. Reads the response body (DNS wire-format answer, max 512 bytes)
 *   7. Parses the answer via ParseDnsResponse() to extract the IP address
 *
 * @see RFC 8484 Section 4.1 — DNS Wire Format (POST method)
 *      https://datatracker.ietf.org/doc/html/rfc8484#section-4.1
 * @see RFC 8484 Section 4.2 — HTTP Response (Content-Type: application/dns-message)
 *      https://datatracker.ietf.org/doc/html/rfc8484#section-4.2
 */
Result<IPAddress, Error> DnsClient::ResolveOverHttp(Span<const CHAR> host, const IPAddress &dnsServerIp, Span<const CHAR> dnsServerName, DnsRecordType dnstype)
{
	// Short-circuit for "localhost" — return loopback without network I/O (RFC 6761 Section 6.3)
	if (StringUtils::Equals<CHAR>(host.Data(), "localhost"))
		return Result<IPAddress, Error>::Ok(IPAddress::LocalHost(dnstype == DnsRecordType::AAAA));

	auto tlsResult = TlsClient::Create(dnsServerName.Data(), dnsServerIp, 443);
	if (!tlsResult)
	{
		LOG_WARNING("Failed to create TLS client for DNS server");
		return Result<IPAddress, Error>::Err(tlsResult, Error::Dns_ConnectFailed);
	}
	auto& tlsClient = tlsResult.Value();

	auto openResult = tlsClient.Open();
	if (!openResult)
	{
		LOG_WARNING("Failed to connect to DNS server");
		return Result<IPAddress, Error>::Err(openResult, Error::Dns_ConnectFailed);
	}

	UINT8 queryBuffer[256];
	auto queryResult = GenerateQuery(host, dnstype, Span<UINT8>(queryBuffer));
	if (!queryResult)
	{
		LOG_WARNING("Failed to generate DNS query");
		return Result<IPAddress, Error>::Err(queryResult, Error::Dns_QueryFailed);
	}
	UINT32 querySize = queryResult.Value();

	auto writeSpan = [&tlsClient](Span<const CHAR> s) -> Result<VOID, Error>
	{
		auto r = tlsClient.Write(s);
		if (!r || r.Value() != s.Size())
			return Result<VOID, Error>::Err(r, Error::Dns_SendFailed);
		return Result<VOID, Error>::Ok();
	};

	CHAR sizeBuf[8];
	StringUtils::UIntToStr(querySize, Span<CHAR>(sizeBuf));

	// Send HTTP/1.1 POST request per RFC 8484 Section 4.1
	if (!writeSpan("POST /dns-query HTTP/1.1\r\nHost: ") ||
		!writeSpan(dnsServerName) ||
		!writeSpan("\r\nContent-Type: application/dns-message\r\nAccept: application/dns-message\r\nContent-Length: ") ||
		!writeSpan(Span<const CHAR>(sizeBuf, StringUtils::Length(sizeBuf))) ||
		!writeSpan("\r\n\r\n"))
	{
		LOG_WARNING("Failed to send DNS query");
		return Result<IPAddress, Error>::Err(Error::Dns_SendFailed);
	}

	auto writeBody = tlsClient.Write(Span<const CHAR>((PCHAR)queryBuffer, querySize));
	if (!writeBody || writeBody.Value() != querySize)
	{
		LOG_WARNING("Failed to send DNS query");
		return Result<IPAddress, Error>::Err(writeBody, Error::Dns_SendFailed);
	}

	auto headerResult = HttpClient::ReadResponseHeaders(tlsClient, 200);
	if (!headerResult)
	{
		LOG_WARNING("DNS server returned non-200 response");
		return Result<IPAddress, Error>::Err(headerResult, Error::Dns_ResponseFailed);
	}
	INT64 contentLength = headerResult.Value();

	if (contentLength <= 0 || contentLength > 512)
	{
		LOG_WARNING("Invalid or missing Content-Length header");
		return Result<IPAddress, Error>::Err(Error::Dns_ResponseFailed);
	}

	UINT8 binaryResponse[512];
	UINT32 totalRead = 0;
	while (totalRead < (UINT32)contentLength)
	{
		auto readResult = tlsClient.Read(Span<CHAR>((PCHAR)(binaryResponse + totalRead), (UINT32)contentLength - totalRead));
		if (!readResult || readResult.Value() <= 0)
		{
			LOG_WARNING("Failed to read DNS binary response");
			return Result<IPAddress, Error>::Err(readResult, Error::Dns_ResponseFailed);
		}
		totalRead += (UINT32)readResult.Value();
	}

	auto parseResult = ParseDnsResponse(Span<const UINT8>(binaryResponse, (USIZE)contentLength));
	if (!parseResult)
	{
		LOG_WARNING("Failed to parse DNS response");
		return Result<IPAddress, Error>::Err(parseResult, Error::Dns_ParseFailed);
	}

	return parseResult;
}

/**
 * @brief Resolves a hostname via Cloudflare's DNS-over-HTTPS service
 * @param host Hostname to resolve
 * @param dnstype Record type — A (IPv4) or AAAA (IPv6)
 * @return Ok(IPAddress) on success, or Err if both Cloudflare servers fail
 *
 * @details Uses Cloudflare's public DoH endpoints:
 *   - Primary:   1.1.1.1 (0x01010101)
 *   - Secondary: 1.0.0.1 (0x01000001)
 *   - SNI: one.one.one.one
 *   - Endpoint: POST https://one.one.one.one/dns-query
 *
 * @see https://developers.cloudflare.com/1.1.1.1/encryption/dns-over-https/
 */
Result<IPAddress, Error> DnsClient::CloudflareResolve(Span<const CHAR> host, DnsRecordType dnstype)
{
	const CHAR serverName[] = "one.one.one.one";
	const IPAddress ips[] = {IPAddress::FromIPv4(0x01010101), IPAddress::FromIPv4(0x01000001)};
	return ResolveWithFallback(host, Span(ips), Span<const CHAR>(serverName, sizeof(serverName) - 1), dnstype);
}

/**
 * @brief Resolves a hostname via Google's DNS-over-HTTPS service
 * @param host Hostname to resolve
 * @param dnstype Record type — A (IPv4) or AAAA (IPv6)
 * @return Ok(IPAddress) on success, or Err if both Google servers fail
 *
 * @details Uses Google's public DoH endpoints:
 *   - Primary:   8.8.8.8 (0x08080808)
 *   - Secondary: 8.8.4.4 (0x08080404)
 *   - SNI: dns.google
 *   - Endpoint: POST https://dns.google/dns-query
 *
 * @see https://developers.google.com/speed/public-dns/docs/doh
 */
Result<IPAddress, Error> DnsClient::GoogleResolve(Span<const CHAR> host, DnsRecordType dnstype)
{
	const CHAR serverName[] = "dns.google";
	const IPAddress ips[] = {IPAddress::FromIPv4(0x08080808), IPAddress::FromIPv4(0x08080404)};
	return ResolveWithFallback(host, Span(ips), Span<const CHAR>(serverName, sizeof(serverName) - 1), dnstype);
}

/**
 * @brief Resolves a hostname to an IP address with automatic provider and protocol fallback
 * @param host Hostname to resolve
 * @param dnstype Record type — A (IPv4) or AAAA (IPv6), defaults to AAAA
 * @return Ok(IPAddress) on success, or Err if all resolution attempts fail
 *
 * @details Primary entry point for DNS resolution. Attempts resolution in the following order:
 *   1. Cloudflare DoH (1.1.1.1 → 1.0.0.1)
 *   2. Google DoH (8.8.8.8 → 8.8.4.4)
 *   3. If dnstype was AAAA (IPv6) and all above failed, retries with A (IPv4):
 *      a. Cloudflare DoH (1.1.1.1 → 1.0.0.1) with A record
 *      b. Google DoH (8.8.8.8 → 8.8.4.4) with A record
 *
 * The AAAA→A fallback handles environments without IPv6 connectivity or hosts
 * that only have A records.
 */
Result<IPAddress, Error> DnsClient::Resolve(Span<const CHAR> host, DnsRecordType dnstype)
{
	LOG_DEBUG("Resolve(host: %s) called", host.Data());

	auto result = CloudflareResolve(host, dnstype);
	if (!result)
		result = GoogleResolve(host, dnstype);

	if (!result && dnstype == DnsRecordType::AAAA)
	{
		LOG_DEBUG("IPv6 resolution failed, falling back to IPv4 (A) for %s", host.Data());
		result = CloudflareResolve(host, DnsRecordType::A);
		if (!result)
			result = GoogleResolve(host, DnsRecordType::A);
	}

	return result;
}
