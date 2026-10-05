#pragma once

#include "lib/runtime.h"
#include "tests.h"

// =============================================================================
// Compile-Time (constexpr) Verification
// =============================================================================
// These static_assert tests verify that IPAddress operations are fully evaluated
// at compile time, producing no .rdata or data section entries.

// Default constructor produces Invalid
static_assert(!IPAddress().IsValid());
static_assert(!IPAddress().IsIPv4());
static_assert(!IPAddress().IsIPv6());
static_assert(IPAddress().GetVersion() == IPVersion::Invalid);

// FromIPv4 factory
static_assert(IPAddress::FromIPv4(0x0100007F).IsIPv4());
static_assert(IPAddress::FromIPv4(0x0100007F).IsValid());
static_assert(!IPAddress::FromIPv4(0x0100007F).IsIPv6());
static_assert(IPAddress::FromIPv4(0x0100007F).ToIPv4() == 0x0100007F);

// FromIPv6 factory
constexpr UINT8 kTestIPv6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
static_assert(IPAddress::FromIPv6(kTestIPv6).IsIPv6());
static_assert(IPAddress::FromIPv6(kTestIPv6).IsValid());
static_assert(!IPAddress::FromIPv6(kTestIPv6).IsIPv4());
static_assert(IPAddress::FromIPv6(kTestIPv6).ToIPv4() == 0xFFFFFFFF);
static_assert(IPAddress::FromIPv6(kTestIPv6).ToIPv6() != nullptr);

// Invalid factory
static_assert(!IPAddress::Invalid().IsValid());
static_assert(IPAddress::Invalid().GetVersion() == IPVersion::Invalid);

// LocalHost IPv4
static_assert(IPAddress::LocalHost().IsIPv4());
static_assert(IPAddress::LocalHost().ToIPv4() == 0x0100007F);

// LocalHost IPv6
static_assert(IPAddress::LocalHost(true).IsIPv6());
static_assert(IPAddress::LocalHost(true).IsValid());

// Equality operator
static_assert(IPAddress::FromIPv4(0x01010101) == IPAddress::FromIPv4(0x01010101));
static_assert(!(IPAddress::FromIPv4(0x01010101) == IPAddress::FromIPv4(0x08080808)));
static_assert(IPAddress::FromIPv4(0x01010101) != IPAddress::FromIPv4(0x08080808));
static_assert(IPAddress::Invalid() == IPAddress::Invalid());
static_assert(IPAddress::FromIPv6(kTestIPv6) == IPAddress::FromIPv6(kTestIPv6));
static_assert(IPAddress::FromIPv4(0x01010101) != IPAddress::Invalid());

// Copy constructor
static_assert(IPAddress(IPAddress::FromIPv4(0xC0A80001)).ToIPv4() == 0xC0A80001);
static_assert(IPAddress(IPAddress::FromIPv6(kTestIPv6)).IsIPv6());

// Assignment (verified through constexpr lambda)
static_assert([]() constexpr
{
	IPAddress a = IPAddress::FromIPv4(0x01010101);
	IPAddress b;
	b = a;
	return b.ToIPv4() == 0x01010101 && b.IsIPv4();
}());

static_assert([]() constexpr
{
	IPAddress a = IPAddress::FromIPv6(kTestIPv6);
	IPAddress b;
	b = a;
	return b.IsIPv6() && b == a;
}());

// =============================================================================
// IPAddress Tests - Runtime Validation
// =============================================================================

class IPAddressTests
{
private:
	static BOOL TestConstexprSuite()
	{
		BOOL allPassed = true;

		// --- IPv4 ---
		{
			IPAddress ip = IPAddress::FromIPv4(0x0100007F);
			BOOL passed = ip.IsIPv4() && ip.ToIPv4() == 0x0100007F && !ip.IsIPv6();

			if (passed)
				LOG_INFO("  PASSED: constexpr IPv4 construction");
			else
			{
				LOG_ERROR("  FAILED: constexpr IPv4 construction");
				allPassed = false;
			}
		}

		// --- IPv6 ---
		{
			const UINT8 addr[] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
			IPAddress ip = IPAddress::FromIPv6(addr);
			BOOL passed = ip.IsIPv6() && !ip.IsIPv4() && ip.IsValid();

			if (passed)
				LOG_INFO("  PASSED: constexpr IPv6 construction");
			else
			{
				LOG_ERROR("  FAILED: constexpr IPv6 construction");
				allPassed = false;
			}
		}

		// --- LocalHost ---
		{
			IPAddress v4 = IPAddress::LocalHost();
			IPAddress v6 = IPAddress::LocalHost(true);
			BOOL passed = v4.IsIPv4() && v4.ToIPv4() == 0x0100007F && v6.IsIPv6();

			if (passed)
				LOG_INFO("  PASSED: constexpr LocalHost");
			else
			{
				LOG_ERROR("  FAILED: constexpr LocalHost");
				allPassed = false;
			}
		}

		// --- Equality ---
		{
			IPAddress a = IPAddress::FromIPv4(0x01010101);
			IPAddress b = IPAddress::FromIPv4(0x01010101);
			IPAddress c = IPAddress::FromIPv4(0x08080808);
			BOOL passed = (a == b) && !(a == c) && (a != c);

			if (passed)
				LOG_INFO("  PASSED: constexpr equality operators");
			else
			{
				LOG_ERROR("  FAILED: constexpr equality operators");
				allPassed = false;
			}
		}

		// --- Copy ---
		{
			IPAddress original = IPAddress::FromIPv4(0xC0A80001);
			IPAddress copy(original);
			BOOL passed = copy.ToIPv4() == original.ToIPv4() && copy == original;

			if (passed)
				LOG_INFO("  PASSED: constexpr copy constructor");
			else
			{
				LOG_ERROR("  FAILED: constexpr copy constructor");
				allPassed = false;
			}
		}

		// --- Invalid ---
		{
			IPAddress inv = IPAddress::Invalid();
			IPAddress def;
			BOOL passed = !inv.IsValid() && !inv.IsIPv4() && !inv.IsIPv6() && !def.IsValid() && (inv == def);

			if (passed)
				LOG_INFO("  PASSED: constexpr Invalid factory");
			else
			{
				LOG_ERROR("  FAILED: constexpr Invalid factory");
				allPassed = false;
			}
		}

		return allPassed;
	}

	// Valid IPv4 literals: every form must parse and ToString must reproduce the
	// canonical dotted-decimal form (byte-level round-trip, endian-independent)
	static BOOL TestFromStringIPv4Suite()
	{
		struct Case
		{
			PCCHAR input;
			PCCHAR expected;
		};
		Case cases[] = {
			{"0.0.0.0", "0.0.0.0"},
			{"255.255.255.255", "255.255.255.255"},
			{"127.0.0.1", "127.0.0.1"},
			{"1.2.3.4", "1.2.3.4"},
			{"10.0.255.1", "10.0.255.1"},
			{"192.168.1.1", "192.168.1.1"},
			// Characterization: a leading-zero octet reads as decimal ("010" -> 10), which
			// diverges from inet_pton (rejects) and inet_aton (octal 8). Pinned deliberately
			// so that any future tightening must update this test consciously.
			{"010.1.1.1", "10.1.1.1"},
		};

		BOOL allPassed = true;

		for (USIZE i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		{
			BOOL passed = true;

			auto result = IPAddress::FromString(cases[i].input);
			if (!result || !result.Value().IsIPv4() || result.Value().IsIPv6())
				passed = false;

			if (passed)
			{
				CHAR buffer[64]{};
				auto toStrResult = result.Value().ToString(Span<CHAR>(buffer));
				if (!toStrResult || !StringUtils::Equals((PCCHAR)buffer, cases[i].expected))
				{
					LOG_ERROR("FromString IPv4 '%s': got '%s', expected '%s'", cases[i].input, buffer, cases[i].expected);
					passed = false;
				}
			}

			if (passed)
				LOG_INFO("  PASSED: FromString IPv4 '%s'", cases[i].input);
			else
			{
				LOG_ERROR("  FAILED: FromString IPv4 '%s'", cases[i].input);
				allPassed = false;
			}
		}

		return allPassed;
	}

	// Malformed IPv4 literals: every form must be rejected (Err), never partially parsed
	static BOOL TestFromStringIPv4InvalidSuite()
	{
		BOOL allPassed = true;

		PCCHAR invalid[] = {
			"",                 // empty
			"256.1.1.1",        // octet out of range
			"1.2.3.256",        // last octet out of range
			"999.999.999.999",  // all octets out of range
			"192.168.1",        // too few octets
			"1.2.3",            // too few octets
			"1.2.3.4.5",        // too many octets
			"1.2.3.4.",         // trailing dot = empty 5th octet
			".1.2.3.4",         // leading dot = empty first octet
			"1..2.3",           // empty middle octet
			".",                // dot alone
			"1234.1.1.1",       // 4-digit octet
			"1.2.3.4444",       // 4-digit last octet
			"abc.def.ghi.jkl",  // non-digits
			"192.168.1.x",      // non-digit tail
			"-1.2.3.4",         // sign
			"+1.2.3.4",         // sign
			" 1.2.3.4",         // leading space
			"1.2.3.4 ",         // trailing space
			"1,2.3.4",          // comma separator
		};

		for (USIZE i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		{
			if (IPAddress::FromString(invalid[i]))
			{
				LOG_ERROR("FromString accepted invalid IPv4 '%s'", invalid[i]);
				allPassed = false;
			}
		}

		// nullptr must be rejected, not crash
		if (IPAddress::FromString(nullptr))
		{
			LOG_ERROR("FromString accepted nullptr");
			allPassed = false;
		}

		if (allPassed)
			LOG_INFO("  PASSED: FromString rejects invalid IPv4 (%d cases)", (INT32)(sizeof(invalid) / sizeof(invalid[0])) + 1);
		else
			LOG_ERROR("  FAILED: FromString rejects invalid IPv4");

		return allPassed;
	}

	// Valid IPv6 literals against the strict RFC 4291 Section 2.2 grammar:
	// groups of 1-4 hex digits, "::" at most once, full-string consumption,
	// exactly 8 groups without "::" and at most 7 with it. Expected values are
	// compared byte-wise via ToIPv6(), independent of host endianness.
	static BOOL TestFromStringIPv6Suite()
	{
		struct Case
		{
			PCCHAR input;
			UINT8 expected[16];
		};
		Case cases[] = {
			{"::", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
			{"::1", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
			{"1::", {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
			{"1::2", {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}},
			{"::ffff", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff}},
			{"2001:db8::1", {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
			// Fully written-out form of 2001:db8::1 must yield the same bytes
			{"2001:0db8:0000:0000:0000:0000:0000:0001", {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
			{"0:0:0:0:0:0:0:0", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
			{"fe80::1", {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
			// Hex digits are case-insensitive
			{"FE80::1", {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
			{"2001:DB8::1", {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
			// "::" after 7 groups compresses only the final group
			{"1:2:3:4:5:6:7::", {0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 0}},
			// Leading "::" before 7 groups compresses only the first
			{"::1:2:3:4:5:6:7", {0, 0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7}},
		};

		BOOL allPassed = true;

		for (USIZE i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		{
			BOOL passed = true;

			auto result = IPAddress::FromString(cases[i].input);
			if (!result || !result.Value().IsIPv6() || result.Value().IsIPv4())
				passed = false;

			if (passed && Memory::Compare(result.Value().ToIPv6(), cases[i].expected, 16) != 0)
			{
				LOG_ERROR("FromString IPv6 '%s' parsed to wrong bytes", cases[i].input);
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: FromString IPv6 '%s'", cases[i].input);
			else
			{
				LOG_ERROR("  FAILED: FromString IPv6 '%s'", cases[i].input);
				allPassed = false;
			}
		}

		// --- IPv6 equality ---
		{
			auto r1 = IPAddress::FromString((PCCHAR)"2001:db8::1");
			auto r2 = IPAddress::FromString((PCCHAR)"2001:db8::1");
			auto r3 = IPAddress::FromString((PCCHAR)"2001:db8::2");

			BOOL passed = r1 && r2 && r3 && (r1.Value() == r2.Value()) && !(r1.Value() == r3.Value());

			if (passed)
				LOG_INFO("  PASSED: IPv6 equality comparison");
			else
			{
				LOG_ERROR("  FAILED: IPv6 equality comparison");
				allPassed = false;
			}
		}

		return allPassed;
	}

	// Malformed IPv6 literals: strict grammar violations must all be rejected.
	// A lax parser here would silently mis-resolve mistyped literal hosts in
	// DnsClient::Resolve (the short-circuit trusts FromString).
	static BOOL TestFromStringIPv6InvalidSuite()
	{
		BOOL allPassed = true;

		PCCHAR invalid[] = {
			"1:2",               // too few groups, no "::"
			"1:2:3:4:5:6:7:8:9",  // more than 8 groups
			"12345::1",          // 5 hex digits in a group
			"deadbeef::1",       // 8 hex digits before "::" (5th digit rejected)
			":1:2",              // leading single colon
			"1:",                // trailing single colon
			"1:2:3:4:5:6:7:8:",  // trailing single colon after 8 complete groups
			"::1:",              // trailing single colon after a leading "::"
			":",                 // lone colon
			":::",               // three colons
			"1::2::3",           // two "::"
			"::1:2:3:4:5:6:7:8", // leading "::" plus 8 groups = 9
			"1:2:3:4:5:6:7::8",  // 8 groups plus "::" = 9
			"2001:db8::g1",      // non-hex digit
			"hello::1",          // non-hex letters
			"2001:db8 ::1",      // embedded space
			"::1 ",              // trailing space
			// The embedded dotted-decimal IPv4 tail is a valid RFC 4291 Section 2.2 address
			// form (form 3, "x:x:x:x:x:x:d.d.d.d"), but this parser supports the hex-groups
			// forms only — pins current rejection; adding that form means updating this row.
			"::ffff:192.0.2.1",
			"", // empty (delegates to the shared size check)
		};

		for (USIZE i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		{
			if (IPAddress::FromString(invalid[i]))
			{
				LOG_ERROR("FromString accepted invalid IPv6 '%s'", invalid[i]);
				allPassed = false;
			}
		}

		if (allPassed)
			LOG_INFO("  PASSED: FromString rejects invalid IPv6 (%d cases)", (INT32)(sizeof(invalid) / sizeof(invalid[0])));
		else
			LOG_ERROR("  FAILED: FromString rejects invalid IPv6");

		return allPassed;
	}

	// The Span overload must be bounded by Size(), never by NUL terminators —
	// DnsClient::Resolve receives non-terminated host sub-spans
	static BOOL TestFromStringSpanSuite()
	{
		BOOL allPassed = true;

		// --- Host followed by ":port" — span covers only the address ---
		{
			CHAR hostPort[] = "192.168.1.1:8080";
			auto result = IPAddress::FromString(Span<const CHAR>(hostPort, 11));
			BOOL passed = result && result.Value().IsIPv4();

			if (passed)
			{
				CHAR buffer[64]{};
				auto toStrResult = result.Value().ToString(Span<CHAR>(buffer));
				passed = toStrResult && StringUtils::Equals((PCCHAR)buffer, (PCCHAR)"192.168.1.1");
			}

			if (passed)
				LOG_INFO("  PASSED: span-bounded IPv4 with trailing ':port'");
			else
			{
				LOG_ERROR("  FAILED: span-bounded IPv4 with trailing ':port'");
				allPassed = false;
			}
		}

		// --- Embedded NUL terminates nothing: Size() is the only bound ---
		{
			CHAR embedded[] = "127.0.0.1\0garbage";
			auto result = IPAddress::FromString(Span<const CHAR>(embedded, 9));
			BOOL passed = result && result.Value().IsIPv4();

			if (passed)
			{
				CHAR buffer[64]{};
				auto toStrResult = result.Value().ToString(Span<CHAR>(buffer));
				passed = toStrResult && StringUtils::Equals((PCCHAR)buffer, (PCCHAR)"127.0.0.1");
			}

			if (passed)
				LOG_INFO("  PASSED: span-bounded IPv4 past an embedded NUL");
			else
			{
				LOG_ERROR("  FAILED: span-bounded IPv4 past an embedded NUL");
				allPassed = false;
			}
		}

		// --- Garbage inside the span bounds must be rejected ---
		{
			CHAR trailing[] = "127.0.0.1JUNK";
			if (IPAddress::FromString(Span<const CHAR>(trailing, 13)))
			{
				LOG_ERROR("Span FromString accepted trailing garbage after an IPv4 literal");
				allPassed = false;
			}
		}

		// --- A NUL inside the span is just an invalid character ---
		{
			CHAR nulInside[] = "127.0\0.0.1";
			if (IPAddress::FromString(Span<const CHAR>(nulInside, 9)))
			{
				LOG_ERROR("Span FromString accepted a NUL inside an IPv4 literal");
				allPassed = false;
			}
		}

		// --- IPv6 host followed by a path — span covers only the address ---
		{
			CHAR hostPath[] = "2001:db8::1/api";
			auto result = IPAddress::FromString(Span<const CHAR>(hostPath, 11));
			BOOL passed = result && result.Value().IsIPv6();

			if (passed)
			{
				const UINT8 expected[] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
				passed = Memory::Compare(result.Value().ToIPv6(), expected, 16) == 0;
			}

			if (passed)
				LOG_INFO("  PASSED: span-bounded IPv6 with trailing '/path'");
			else
			{
				LOG_ERROR("  FAILED: span-bounded IPv6 with trailing '/path'");
				allPassed = false;
			}
		}

		// --- Path inside the span bounds must be rejected ---
		{
			CHAR hostPath[] = "2001:db8::1/api";
			if (IPAddress::FromString(Span<const CHAR>(hostPath, 15)))
			{
				LOG_ERROR("Span FromString accepted trailing '/path' after an IPv6 literal");
				allPassed = false;
			}
		}

		// --- IPv6 past an embedded NUL ---
		{
			CHAR embedded[] = "::1\0garbage";
			auto result = IPAddress::FromString(Span<const CHAR>(embedded, 3));
			BOOL passed = result && result.Value().IsIPv6();

			if (passed)
			{
				const UINT8 expected[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
				passed = Memory::Compare(result.Value().ToIPv6(), expected, 16) == 0;
			}

			if (passed)
				LOG_INFO("  PASSED: span-bounded IPv6 past an embedded NUL");
			else
			{
				LOG_ERROR("  FAILED: span-bounded IPv6 past an embedded NUL");
				allPassed = false;
			}
		}

		// --- Zero-length span is rejected, not crashed on ---
		{
			CHAR anything[] = "192.168.1.1";
			if (IPAddress::FromString(Span<const CHAR>(anything, 0)))
			{
				LOG_ERROR("Span FromString accepted a zero-length span");
				allPassed = false;
			}
		}

		if (allPassed)
			LOG_INFO("  PASSED: FromString span bounds");
		else
			LOG_ERROR("  FAILED: FromString span bounds");

		return allPassed;
	}

	// ToString: canonical output forms, a re-parse round-trip, and buffer-size
	// rejection. IPv6 renders as the full uncompressed 8-group form with minimal
	// lowercase hex per group (no "::" compression) — "::1" becomes "0:0:0:0:0:0:0:1".
	static BOOL TestToStringSuite()
	{
		BOOL allPassed = true;

		// --- Exact IPv6 output forms ---
		{
			struct Case
			{
				PCCHAR input;
				PCCHAR expected;
			};
			Case cases[] = {
				{"::1", "0:0:0:0:0:0:0:1"},
				{"1::2", "1:0:0:0:0:0:0:2"},
				{"2001:db8::1", "2001:db8:0:0:0:0:0:1"},
				// Uppercase input canonicalizes to lowercase output
				{"FE80::A", "fe80:0:0:0:0:0:0:a"},
			};

			for (USIZE i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
			{
				BOOL passed = true;
				auto result = IPAddress::FromString(cases[i].input);
				if (!result)
					passed = false;

				if (passed)
				{
					CHAR buffer[64]{};
					auto toStrResult = result.Value().ToString(Span<CHAR>(buffer));
					if (!toStrResult || !StringUtils::Equals((PCCHAR)buffer, cases[i].expected))
					{
						LOG_ERROR("ToString: '%s' rendered as '%s', expected '%s'", cases[i].input, buffer, cases[i].expected);
						passed = false;
					}
				}

				if (passed)
					LOG_INFO("  PASSED: ToString IPv6 '%s'", cases[i].input);
				else
				{
					LOG_ERROR("  FAILED: ToString IPv6 '%s'", cases[i].input);
					allPassed = false;
				}
			}
		}

		// --- Round-trip: parse -> render -> parse must be a fixed point ---
		{
			PCCHAR inputs[] = {"::", "::1", "1::", "1::2", "::ffff", "2001:db8::1", "fe80::1", "::1:2:3:4:5:6:7", "0.0.0.0", "127.0.0.1", "192.168.1.1", "255.255.255.255"};

			for (USIZE i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++)
			{
				BOOL passed = true;

				auto first = IPAddress::FromString(inputs[i]);
				if (!first)
					passed = false;

				if (passed)
				{
					CHAR once[64]{};
					auto onceResult = first.Value().ToString(Span<CHAR>(once));
					if (!onceResult)
						passed = false;

					if (passed)
					{
						auto second = IPAddress::FromString((PCCHAR)once);
						if (!second)
							passed = false;

						if (passed)
						{
							CHAR twice[64]{};
							auto twiceResult = second.Value().ToString(Span<CHAR>(twice));
							if (!twiceResult || !StringUtils::Equals((PCCHAR)twice, (PCCHAR)once))
								passed = false;
						}
					}
				}

				if (!passed)
				{
					LOG_ERROR("ToString round-trip failed for '%s'", inputs[i]);
					allPassed = false;
				}
			}

			if (allPassed)
				LOG_INFO("  PASSED: ToString round-trip (%d cases)", (INT32)(sizeof(inputs) / sizeof(inputs[0])));
		}

		// --- Buffer too small is rejected, never truncated ---
		{
			BOOL passed = true;

			auto v4 = IPAddress::FromString((PCCHAR)"255.255.255.255");
			CHAR small4[15]; // needs 16 for "255.255.255.255\0"
			if (!v4 || v4.Value().ToString(Span<CHAR>(small4)))
				passed = false;

			auto v6 = IPAddress::FromString((PCCHAR)"2001:db8:0:0:0:0:0:1");
			CHAR small6[39]; // needs 40 for the longest uncompressed form
			if (!v6 || v6.Value().ToString(Span<CHAR>(small6)))
				passed = false;

			// Invalid address has no textual form
			CHAR buffer[64]{};
			if (IPAddress::Invalid().ToString(Span<CHAR>(buffer)))
				passed = false;

			// Zero-length span is rejected up front
			auto any = IPAddress::FromString((PCCHAR)"127.0.0.1");
			if (!any || any.Value().ToString(Span<CHAR>(buffer, 0)))
				passed = false;

			if (passed)
				LOG_INFO("  PASSED: ToString rejects small buffers and invalid addresses");
			else
			{
				LOG_ERROR("  FAILED: ToString rejects small buffers and invalid addresses");
				allPassed = false;
			}
		}

		return allPassed;
	}

public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running IPAddress Tests...");

		RunTest(allPassed, &TestConstexprSuite, "Constexpr suite");
		RunTest(allPassed, &TestFromStringIPv4Suite, "FromString IPv4 suite");
		RunTest(allPassed, &TestFromStringIPv4InvalidSuite, "FromString IPv4 rejection suite");
		RunTest(allPassed, &TestFromStringIPv6Suite, "FromString IPv6 suite");
		RunTest(allPassed, &TestFromStringIPv6InvalidSuite, "FromString IPv6 rejection suite");
		RunTest(allPassed, &TestFromStringSpanSuite, "FromString span-bounds suite");
		RunTest(allPassed, &TestToStringSuite, "ToString suite");

		if (allPassed)
			LOG_INFO("All IPAddress tests passed!");
		else
			LOG_ERROR("Some IPAddress tests failed!");

		return allPassed;
	}
};
