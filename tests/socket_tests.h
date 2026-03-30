#pragma once

#include "lib/runtime.h"
#include "tests.h"

// =============================================================================
// Socket Tests - AFD Socket Implementation Validation
// Server: one.one.one.one (1.1.1.1) - Cloudflare Public DNS
//
// Tests are consolidated to use minimal connections to reduce network overhead.
// =============================================================================

class SocketTests
{
private:
// Test server IP address: 1.1.1.1 (one.one.one.one)
#define TEST_SERVER_IP 0x01010101

	// Connection 1+2: All IPv4 socket tests on minimal connections
	static BOOL TestIPv4Suite()
	{
		LOG_INFO("Connecting to one.one.one.one:80 (IPv4)...");

		// --- Creation + connection + HTTP GET on a single connection ---
		auto createResult = Socket::Create(IPAddress::FromIPv4(TEST_SERVER_IP), 80);
		if (!createResult)
		{
			LOG_ERROR("Socket creation failed (error: %e)", createResult.Error());
			return false;
		}
		Socket &sock = createResult.Value();
		LOG_INFO("  PASSED: Socket creation");

		auto openResult = sock.Open();
		if (!openResult)
		{
			LOG_ERROR("Socket connection failed (error: %e)", openResult.Error());
			(VOID)sock.Close();
			return false;
		}
		LOG_INFO("  PASSED: Socket connection (HTTP:80)");

		const CHAR request[] = "GET / HTTP/1.1\r\nHost: one.one.one.one\r\nConnection: close\r\n\r\n";
		constexpr USIZE requestLen = sizeof(request) - 1;
		auto writeResult = sock.Write(Span<const CHAR>(request, requestLen));

		if (!writeResult)
		{
			LOG_ERROR("Failed to send HTTP request (error: %e)", writeResult.Error());
			(VOID)sock.Close();
			return false;
		}
		if (writeResult.Value() != requestLen)
		{
			LOG_ERROR("Incomplete HTTP request (sent %d/%d bytes)", writeResult.Value(), requestLen);
			(VOID)sock.Close();
			return false;
		}

		CHAR buffer[512];
		Memory::Zero(buffer, sizeof(buffer));
		auto readResult = sock.Read(Span<CHAR>(buffer, sizeof(buffer) - 1));

		if (!readResult || readResult.Value() <= 0)
		{
			if (!readResult)
				LOG_ERROR("Failed to receive HTTP response (error: %e)", readResult.Error());
			else
				LOG_ERROR("Failed to receive HTTP response (zero bytes)");
			(VOID)sock.Close();
			return false;
		}
		LOG_INFO("  PASSED: HTTP GET request");

		(VOID)sock.Close();

		// --- Sequential reconnection test (verifies create-connect-send cycle works repeatedly) ---
		{
			auto createResult2 = Socket::Create(IPAddress::FromIPv4(TEST_SERVER_IP), 80);
			if (!createResult2)
			{
				LOG_ERROR("Sequential connection: socket creation failed (error: %e)", createResult2.Error());
				return false;
			}
			Socket &sock2 = createResult2.Value();

			auto openResult2 = sock2.Open();
			if (!openResult2)
			{
				LOG_ERROR("Sequential connection failed (error: %e)", openResult2.Error());
				return false;
			}

			const CHAR request2[] = "GET / HTTP/1.0\r\n\r\n";
			constexpr USIZE requestLen2 = sizeof(request2) - 1;
			auto writeResult2 = sock2.Write(Span<const CHAR>(request2, requestLen2));

			if (!writeResult2)
			{
				LOG_ERROR("Sequential connection: failed to send request (error: %e)", writeResult2.Error());
				(VOID)sock2.Close();
				return false;
			}
			if (writeResult2.Value() != requestLen2)
			{
				LOG_ERROR("Sequential connection: incomplete send (%d/%d bytes)", writeResult2.Value(), requestLen2);
				(VOID)sock2.Close();
				return false;
			}

			CHAR buffer2[128];
			Memory::Zero(buffer2, sizeof(buffer2));
			auto readResult2 = sock2.Read(Span<CHAR>(buffer2, sizeof(buffer2) - 1));

			if (!readResult2 || readResult2.Value() <= 0)
			{
				if (!readResult2)
					LOG_ERROR("Sequential connection: failed to receive response (error: %e)", readResult2.Error());
				else
					LOG_ERROR("Sequential connection: received zero bytes");
				(VOID)sock2.Close();
				return false;
			}

			(VOID)sock2.Close();
		}
		LOG_INFO("  PASSED: Sequential reconnection");

		return true;
	}

	// IP address conversion (no network)
	static BOOL TestIpConversion()
	{
		LOG_INFO("Test: IP Address Conversion");

		auto ipStr = "1.1.1.1";
		auto convertedResult = IPAddress::FromString((PCCHAR)ipStr);

		if (!convertedResult)
		{
			LOG_ERROR("IP conversion failed for valid IP");
			return false;
		}
		IPAddress convertedIp = convertedResult.Value();

		if (convertedIp.ToIPv4() != TEST_SERVER_IP)
		{
			LOG_ERROR("IP conversion mismatch: expected 0x%08X, got 0x%08X", TEST_SERVER_IP, convertedIp.ToIPv4());
			return false;
		}

		LOG_INFO("IP conversion successful: %s -> 0x%08X", (PCCHAR)ipStr, convertedIp.ToIPv4());

		LOG_INFO("  [D1] Testing invalid IP: 256.1.1.1");
		auto invalidIp1 = "256.1.1.1";
		auto parseResult1 = IPAddress::FromString((PCCHAR)invalidIp1);
		if (parseResult1)
		{
			LOG_ERROR("Failed to reject invalid IP: %s", (PCCHAR)invalidIp1);
			return false;
		}

		LOG_INFO("  [D2] Testing invalid IP: 192.168.1");
		auto invalidIp2 = "192.168.1";
		auto parseResult2 = IPAddress::FromString((PCCHAR)invalidIp2);
		if (parseResult2)
		{
			LOG_ERROR("Failed to reject invalid IP: %s", (PCCHAR)invalidIp2);
			return false;
		}

		LOG_INFO("  [D3] Testing invalid IP: abc.def.ghi.jkl");
		auto invalidIp3 = "abc.def.ghi.jkl";
		auto parseResult3 = IPAddress::FromString((PCCHAR)invalidIp3);
		if (parseResult3)
		{
			LOG_ERROR("Failed to reject invalid IP: %s", (PCCHAR)invalidIp3);
			return false;
		}

		LOG_INFO("  [D4] Testing IPv6: 2001:db8::1");
		auto ipv6Str = "2001:db8::1";
		LOG_INFO("  [D5] Calling FromString");
		auto ipv6Result = IPAddress::FromString((PCCHAR)ipv6Str);
		LOG_INFO("  [D6] FromString done: ok=%d", (INT32)(BOOL)ipv6Result);
		if (!ipv6Result)
		{
			LOG_ERROR("IPv6 conversion failed for valid IPv6");
			return false;
		}
		IPAddress ipv6Address = ipv6Result.Value();
		if (!ipv6Address.IsIPv6())
		{
			LOG_ERROR("IPv6 conversion returned non-IPv6 address");
			return false;
		}

		LOG_INFO("  [D7] About to format with %%s arg");
		LOG_INFO("IPv6 conversion successful: %s", (PCCHAR)ipv6Str);
		LOG_INFO("Invalid IP rejection tests passed");
		return true;
	}

	// Connection 3: IPv6 socket connection
	static BOOL TestIPv6Connection()
	{
		LOG_INFO("Test: IPv6 Socket Connection (HTTP:80)");

		auto ipv6Str = "2606:4700:4700::1111";
		auto ipv6Result = IPAddress::FromString((PCCHAR)ipv6Str);

		if (!ipv6Result || !ipv6Result.Value().IsIPv6())
		{
			LOG_ERROR("Failed to parse IPv6 address: %s", (PCCHAR)ipv6Str);
			return false;
		}
		IPAddress ipv6Address = ipv6Result.Value();

		auto createResult = Socket::Create(ipv6Address, 80);
		if (!createResult)
		{
			LOG_WARNING("IPv6 socket creation failed (error: %e) (IPv6 may not be available)", createResult.Error());
			return true; // non-fatal: IPv6 may be unavailable
		}
		Socket &sock = createResult.Value();

		auto openResult = sock.Open();
		if (!openResult)
		{
			LOG_WARNING("IPv6 socket connection failed (error: %e) (IPv6 may not be available)", openResult.Error());
			(VOID)sock.Close();
			return true; // non-fatal: IPv6 may be unavailable
		}

		LOG_INFO("IPv6 socket connected successfully to %s:80", (PCCHAR)ipv6Str);

		const CHAR request[] = "GET / HTTP/1.1\r\nHost: one.one.one.one\r\nConnection: close\r\n\r\n";
		constexpr USIZE requestLen = sizeof(request) - 1;
		auto writeResult = sock.Write(Span<const CHAR>(request, requestLen));

		if (!writeResult)
		{
			LOG_ERROR("Failed to send HTTP request over IPv6 (error: %e)", writeResult.Error());
			(VOID)sock.Close();
			return false;
		}
		if (writeResult.Value() != requestLen)
		{
			LOG_ERROR("Incomplete HTTP request over IPv6 (sent %d/%d bytes)", writeResult.Value(), requestLen);
			(VOID)sock.Close();
			return false;
		}

		CHAR buffer[512];
		Memory::Zero(buffer, sizeof(buffer));
		auto readResult = sock.Read(Span<CHAR>(buffer, sizeof(buffer) - 1));

		if (!readResult)
		{
			LOG_ERROR("Failed to receive HTTP response over IPv6 (error: %e)", readResult.Error());
			(VOID)sock.Close();
			return false;
		}
		if (readResult.Value() <= 0)
		{
			LOG_ERROR("Received zero bytes over IPv6");
			(VOID)sock.Close();
			return false;
		}

		(VOID)sock.Close();
		return true;
	}

	// Connection 4: HTTP GET request to httpbin.org (requires DNS)
	static BOOL TestHttpBin()
	{
		auto dnsResult = DnsClient::Resolve("httpbin.org", DnsRecordType::A);
		if (!dnsResult)
		{
			LOG_ERROR("Failed to resolve httpbin.org (error: %e)", dnsResult.Error());
			return false;
		}

		auto createResult = Socket::Create(dnsResult.Value(), 80);
		if (!createResult)
		{
			LOG_ERROR("Socket creation failed for httpbin.org (error: %e)", createResult.Error());
			return false;
		}
		Socket &sock = createResult.Value();
		auto openResult = sock.Open();
		if (!openResult)
		{
			LOG_ERROR("Failed to open socket to httpbin.org (error: %e)", openResult.Error());
			return false;
		}

		const CHAR request[] = "GET /get HTTP/1.1\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n";
		auto writeResult = sock.Write(Span<const CHAR>(request, sizeof(request) - 1));
		if (!writeResult)
		{
			LOG_ERROR("Failed to send HTTP request to httpbin.org (error: %e)", writeResult.Error());
			(VOID)sock.Close();
			return false;
		}
		if (writeResult.Value() != sizeof(request) - 1)
		{
			LOG_ERROR("Incomplete HTTP request to httpbin.org (sent %d/%d bytes)", writeResult.Value(), sizeof(request) - 1);
			(VOID)sock.Close();
			return false;
		}

		CHAR buffer[1024];
		Memory::Zero(buffer, sizeof(buffer));
		auto readResult = sock.Read(Span<CHAR>(buffer, sizeof(buffer) - 1));
		if (!readResult)
		{
			LOG_ERROR("Failed to receive HTTP response from httpbin.org (error: %e)", readResult.Error());
			(VOID)sock.Close();
			return false;
		}
		if (readResult.Value() <= 0)
		{
			LOG_ERROR("Received zero bytes from httpbin.org");
			(VOID)sock.Close();
			return false;
		}

		LOG_INFO("Received %d bytes from httpbin.org", readResult.Value());
		(VOID)sock.Close();
		return true;
	}

public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Socket Tests...");
		LOG_INFO("  Test Server: one.one.one.one (1.1.1.1 / 2606:4700:4700::1111)");

		RunTest(allPassed, &TestIPv4Suite, "IPv4 socket suite (create, connect, HTTP GET, reconnect)");
		RunTest(allPassed, &TestIpConversion, "IP address conversion");
		RunTest(allPassed, &TestIPv6Connection, "IPv6 connection");
		RunTest(allPassed, &TestHttpBin, "HTTP GET request to httpbin.org");

		if (allPassed)
			LOG_INFO("All Socket tests passed!");
		else
			LOG_ERROR("Some Socket tests failed!");

		return allPassed;
	}
};
