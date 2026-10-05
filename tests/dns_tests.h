#pragma once

#include "lib/runtime.h"
#include "tests.h"

// =============================================================================
// DNS Tests - DNS Resolution via DoH (binary wireformat)
//
// Tests are consolidated to reduce the number of DNS queries.
// =============================================================================

class DnsTests
{
private:
	// Cloudflare resolver: localhost (A + AAAA) and dns.google (A)
	static BOOL TestCloudflareResolution()
	{
		LOG_INFO("Test: Cloudflare DNS Resolution");

		BOOL allPassed = true;

		// --- Localhost A record ---
		{
			auto result = DnsClient::CloudflareResolve("localhost", DnsRecordType::A);
			if (!result)
			{
				LOG_ERROR("Localhost A resolution failed (error: %e)", result.Error());
				return false;
			}
			auto& ip = result.Value();

			if (ip.ToIPv4() != 0x0100007F)
			{
				LOG_ERROR("Localhost resolution failed: expected 0x0100007F, got 0x%08X", ip.ToIPv4());
				return false;
			}
			LOG_INFO("  PASSED: Localhost A -> 127.0.0.1");
		}

		// --- Localhost AAAA record ---
		{
			auto result6 = DnsClient::CloudflareResolve("localhost", DnsRecordType::AAAA);
			if (!result6)
			{
				LOG_ERROR("Localhost AAAA resolution failed (error: %e)", result6.Error());
				return false;
			}
			auto& ip6 = result6.Value();

			UINT8 expectedIPv6[16]{};
			expectedIPv6[15] = 1;
			if (ip6.IsIPv6() == false || Memory::Compare(ip6.ToIPv6(), expectedIPv6, 16) != 0)
			{
				LOG_ERROR("Localhost IPv6 resolution failed: expected ::1, got different address");
				return false;
			}
			LOG_INFO("  PASSED: Localhost AAAA -> ::1");
		}

		// --- dns.google A record ---
		{
			auto result = DnsClient::CloudflareResolve("dns.google", DnsRecordType::A);
			if (!result)
			{
				LOG_ERROR("Cloudflare DNS resolution failed (error: %e)", result.Error());
				return false;
			}
			auto& ip = result.Value();

			if (ip.ToIPv4() != 0x08080808 && ip.ToIPv4() != 0x04040808)
			{
				LOG_ERROR("Unexpected IP for dns.google: 0x%08X", ip.ToIPv4());
				return false;
			}
			LOG_INFO("  PASSED: dns.google -> 0x%08X", ip.ToIPv4());
		}

		return allPassed;
	}

	// Google resolver: one.one.one.one (A)
	static BOOL TestGoogleResolution()
	{
		LOG_INFO("Test: Google DNS Resolution (one.one.one.one)");

		auto result = DnsClient::GoogleResolve("one.one.one.one", DnsRecordType::A);
		if (!result)
		{
			LOG_ERROR("Google DNS resolution failed (error: %e)", result.Error());
			return false;
		}
		auto& ip = result.Value();

		if (ip.ToIPv4() != 0x01010101 && ip.ToIPv4() != 0x01000001)
		{
			LOG_ERROR("Unexpected IP for one.one.one.one: 0x%08X", ip.ToIPv4());
			return false;
		}

		LOG_INFO("Google resolved one.one.one.one to 0x%08X", ip.ToIPv4());
		return true;
	}

	// Main Resolve function (tries IPv6 first, falls back to IPv4)
	static BOOL TestMainResolve()
	{
		LOG_INFO("Test: Main DNS Resolve Function");

		auto result = DnsClient::Resolve("example.com");
		if (!result)
		{
			LOG_ERROR("Main DNS resolution failed for example.com (error: %e)", result.Error());
			return false;
		}
		LOG_INFO("  PASSED: Resolve example.com");

		return true;
	}

	// IP-literal short-circuit — hermetic, no network I/O involved.
	// Matrix over literal family x requested record type:
	//   - default (AAAA) and explicit AAAA accept either family (the AAAA->A
	//     fallback would surface an IPv4 literal anyway)
	//   - explicit A accepts only IPv4 literals — an IPv6 literal has no A record,
	//     and the IPv4-fallback callers in the HTTP/WebSocket clients rely on that
	//     terminal failure
	static BOOL TestLiteralResolution()
	{
		LOG_INFO("Test: IP-literal short-circuit in Resolve");

		// --- IPv4 literal, default (AAAA) request ---
		auto v4 = DnsClient::Resolve("127.0.0.1");
		if (!v4 || !v4.Value().IsIPv4() || v4.Value().ToIPv4() != 0x0100007F)
		{
			LOG_ERROR("Resolve did not return 127.0.0.1 for the IPv4 literal");
			return false;
		}

		// --- IPv4 literal, explicit A request ---
		auto v4a = DnsClient::Resolve("192.0.2.1", DnsRecordType::A);
		if (!v4a || !v4a.Value().IsIPv4() || v4a.Value().ToIPv4() != 0x010200C0)
		{
			LOG_ERROR("Resolve did not return the IPv4 literal for an explicit A request");
			return false;
		}

		// --- IPv4 literal, explicit AAAA request (either family accepted) ---
		auto v4aaaa = DnsClient::Resolve("192.0.2.1", DnsRecordType::AAAA);
		if (!v4aaaa || !v4aaaa.Value().IsIPv4())
		{
			LOG_ERROR("Resolve did not return the IPv4 literal for an explicit AAAA request");
			return false;
		}

		// --- Boundary IPv4 literals ---
		auto zero = DnsClient::Resolve("0.0.0.0");
		if (!zero || !zero.Value().IsIPv4() || zero.Value().ToIPv4() != 0)
		{
			LOG_ERROR("Resolve did not return the 0.0.0.0 literal");
			return false;
		}

		auto broadcast = DnsClient::Resolve("255.255.255.255", DnsRecordType::A);
		if (!broadcast || !broadcast.Value().IsIPv4() || broadcast.Value().ToIPv4() != 0xFFFFFFFF)
		{
			LOG_ERROR("Resolve did not return the 255.255.255.255 literal");
			return false;
		}

		// --- IPv6 literal, default request ---
		auto v6 = DnsClient::Resolve("::1");
		if (!v6 || !v6.Value().IsIPv6())
		{
			LOG_ERROR("Resolve did not return the IPv6 literal for ::1");
			return false;
		}

		UINT8 expectedLoopback[16]{};
		expectedLoopback[15] = 1;
		if (Memory::Compare(v6.Value().ToIPv6(), expectedLoopback, 16) != 0)
		{
			LOG_ERROR("Resolve(::1) did not round-trip the literal bytes");
			return false;
		}

		// --- IPv6 literals, compressed / full / uppercase hex forms ---
		auto v6Compressed = DnsClient::Resolve("2001:db8::1");
		const UINT8 expectedDoc[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
		if (!v6Compressed || !v6Compressed.Value().IsIPv6() || Memory::Compare(v6Compressed.Value().ToIPv6(), expectedDoc, 16) != 0)
		{
			LOG_ERROR("Resolve did not round-trip the 2001:db8::1 literal bytes");
			return false;
		}

		auto v6Uppercase = DnsClient::Resolve("FE80::1");
		if (!v6Uppercase || !v6Uppercase.Value().IsIPv6() || v6Uppercase.Value().ToIPv6()[0] != 0xfe || v6Uppercase.Value().ToIPv6()[1] != 0x80)
		{
			LOG_ERROR("Resolve did not accept uppercase hex in an IPv6 literal");
			return false;
		}

		// An IPv6 literal has no A record — the IPv4 fallback must not be satisfied by the same v6 address
		auto v6a = DnsClient::Resolve("::1", DnsRecordType::A);
		if (v6a)
		{
			LOG_ERROR("Resolve(::1, A) must fail instead of returning the IPv6 literal");
			return false;
		}

		// --- Non-address record types must never be answered by a literal ---
		// A forward query for PTR/TXT/MX/CNAME/NS on an all-numeric qname cannot
		// succeed, so a literal host fails fast for those types instead of
		// returning its own address
		{
			struct Case
			{
				PCCHAR host;
				DnsRecordType type;
			};
			Case nonAddress[] = {
				{"127.0.0.1", DnsRecordType::PTR},
				{"127.0.0.1", DnsRecordType::TXT},
				{"192.0.2.1", DnsRecordType::MX},
				{"::1", DnsRecordType::PTR},
				{"fe80::1", DnsRecordType::CNAME},
				{"192.0.2.1", DnsRecordType::NS},
			};

			for (USIZE i = 0; i < sizeof(nonAddress) / sizeof(nonAddress[0]); i++)
			{
				auto host = Span<const CHAR>(nonAddress[i].host, StringUtils::Length(nonAddress[i].host));
				if (DnsClient::Resolve(host, nonAddress[i].type))
				{
					LOG_ERROR("Resolve returned an address for a non-address record type (%s)", nonAddress[i].host);
					return false;
				}
			}
		}

		// --- Span-bounded host: the literal match must not rely on NUL termination ---
		CHAR hostWithPort[] = "127.0.0.1:8080";
		auto spanHost = DnsClient::Resolve(Span<const CHAR>(hostWithPort, 9));
		if (!spanHost || !spanHost.Value().IsIPv4() || spanHost.Value().ToIPv4() != 0x0100007F)
		{
			LOG_ERROR("Resolve did not match an IPv4 literal from a non-terminated host span");
			return false;
		}

		LOG_INFO("  PASSED: IP-literal short-circuit");
		return true;
	}

	// "localhost" short-circuit (ResolveOverHttp, RFC 6761 Section 6.3) — hermetic,
	// matched case-insensitively per RFC 1035 Section 2.3.3 before any TLS connect.
	// Covers all three public entry points: Cloudflare, Google, and composite Resolve.
	static BOOL TestLocalhostResolution()
	{
		LOG_INFO("Test: localhost short-circuit (case-insensitive, no network)");

		// --- Cloudflare, uppercase host, A record ---
		auto cfUpper = DnsClient::CloudflareResolve("LOCALHOST", DnsRecordType::A);
		if (!cfUpper || !cfUpper.Value().IsIPv4() || cfUpper.Value().ToIPv4() != 0x0100007F)
		{
			LOG_ERROR("CloudflareResolve did not short-circuit 'LOCALHOST' to 127.0.0.1");
			return false;
		}

		// --- Cloudflare, mixed case, AAAA record ---
		auto cfMixed = DnsClient::CloudflareResolve("LocalHost", DnsRecordType::AAAA);
		if (!cfMixed || !cfMixed.Value().IsIPv6())
		{
			LOG_ERROR("CloudflareResolve did not short-circuit 'LocalHost' (AAAA)");
			return false;
		}

		UINT8 expectedV6[16]{};
		expectedV6[15] = 1;
		if (Memory::Compare(cfMixed.Value().ToIPv6(), expectedV6, 16) != 0)
		{
			LOG_ERROR("CloudflareResolve('LocalHost', AAAA) did not return ::1");
			return false;
		}

		// --- Google entry point shares the same short-circuit ---
		auto googleUpper = DnsClient::GoogleResolve("LOCALHOST", DnsRecordType::A);
		if (!googleUpper || !googleUpper.Value().IsIPv4() || googleUpper.Value().ToIPv4() != 0x0100007F)
		{
			LOG_ERROR("GoogleResolve did not short-circuit 'LOCALHOST' to 127.0.0.1");
			return false;
		}

		// --- Composite entry point reaches the short-circuit through the provider chain ---
		auto composite = DnsClient::Resolve("LoCaLhOsT");
		if (!composite)
		{
			LOG_ERROR("Resolve did not short-circuit a case-mangled 'localhost'");
			return false;
		}

		// --- Non-address record types fail fast: localhost is never forwarded upstream,
		//     for any record type (RFC 6761 Section 6.3) ---
		auto txt = DnsClient::Resolve("localhost", DnsRecordType::TXT);
		if (txt)
		{
			LOG_ERROR("Resolve returned a result for a non-address localhost query");
			return false;
		}

		auto ptr = DnsClient::CloudflareResolve("LOCALHOST", DnsRecordType::PTR);
		if (ptr)
		{
			LOG_ERROR("CloudflareResolve returned a result for a non-address localhost query");
			return false;
		}

		// --- The whole .localhost family is special (RFC 6761 Section 6.3): the root-dot
		//     FQDN form, and any subdomain, answer loopback / fail fast without the wire.
		//     (Dot-boundary negatives like "evillocalhost" cannot be asserted here — a
		//     non-matching name legitimately performs DoH, which would touch the network.) ---
		auto rootDot = DnsClient::CloudflareResolve("localhost.", DnsRecordType::A);
		if (!rootDot || !rootDot.Value().IsIPv4() || rootDot.Value().ToIPv4() != 0x0100007F)
		{
			LOG_ERROR("CloudflareResolve did not answer loopback for the 'localhost.' FQDN form");
			return false;
		}

		auto subdomain = DnsClient::Resolve("service.localhost", DnsRecordType::A);
		if (!subdomain || !subdomain.Value().IsIPv4() || subdomain.Value().ToIPv4() != 0x0100007F)
		{
			LOG_ERROR("Resolve did not answer loopback for a .localhost subdomain");
			return false;
		}

		auto mixedCase = DnsClient::Resolve("Api.LocalHost.", DnsRecordType::AAAA);
		if (!mixedCase || !mixedCase.Value().IsIPv6())
		{
			LOG_ERROR("Resolve did not answer ::1 for a mixed-case .localhost name (AAAA)");
			return false;
		}

		UINT8 expectedSub[16]{};
		expectedSub[15] = 1;
		if (Memory::Compare(mixedCase.Value().ToIPv6(), expectedSub, 16) != 0)
		{
			LOG_ERROR("Resolve returned the wrong address for a .localhost name");
			return false;
		}

		auto subTxt = DnsClient::Resolve("db.localhost", DnsRecordType::TXT);
		if (subTxt)
		{
			LOG_ERROR("Resolve returned a result for a non-address .localhost query");
			return false;
		}

		LOG_INFO("  PASSED: localhost short-circuit");
		return true;
	}

public:
	// Run all DNS tests
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running DNS Tests...");
		LOG_INFO("  Testing DNS resolution via DoH (binary wireformat)");

		RunTest(allPassed, &TestCloudflareResolution, "Cloudflare DNS resolution (localhost + dns.google)");
		RunTest(allPassed, &TestGoogleResolution, "Google DNS resolution");
		RunTest(allPassed, &TestMainResolve, "Main DNS resolve function");
		RunTest(allPassed, &TestLiteralResolution, "IP-literal short-circuit (no network)");
		RunTest(allPassed, &TestLocalhostResolution, "localhost short-circuit (no network)");

		if (allPassed)
			LOG_INFO("All DNS tests passed!");
		else
			LOG_ERROR("Some DNS tests failed!");

		return allPassed;
	}
};
