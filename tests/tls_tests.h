#pragma once

#include "lib/runtime.h"
#include "tests.h"

// =============================================================================
// TLS Tests - TLS 1.3 Implementation Validation
// Server: one.one.one.one (1.1.1.1:443)
//
// Tests are consolidated to use a single TLS connection to reduce handshake overhead.
// =============================================================================

class TlsTests
{
private:
#define TEST_SERVER_IP 0x01010101
#define TLS_PORT 443

	// Helper: send HTTP request and verify response
	static BOOL SendAndVerifyHttp(TlsClient &tls, Span<const CHAR> request, [[maybe_unused]] PCCHAR label)
	{
		auto writeResult = tls.Write(request);
		if (!writeResult)
		{
			LOG_ERROR("Failed to send %s (error: %e)", label, writeResult.Error());
			return false;
		}
		if (writeResult.Value() != request.Size())
		{
			LOG_ERROR("Incomplete send for %s (%d/%d bytes)", label, writeResult.Value(), request.Size());
			return false;
		}

		CHAR buffer[128];
		Memory::Zero(buffer, sizeof(buffer));
		auto readResult = tls.Read(Span<CHAR>(buffer, sizeof(buffer) - 1));
		if (!readResult)
		{
			LOG_ERROR("Failed to receive response for %s (error: %e)", label, readResult.Error());
			return false;
		}
		if (readResult.Value() <= 0)
		{
			LOG_ERROR("Received zero bytes for %s", label);
			return false;
		}

		return true;
	}

	// Single connection: handshake + single message + multiple sequential messages
	static BOOL TestTlsSuite()
	{
		LOG_INFO("Connecting to one.one.one.one:443 (TLS 1.3)...");

		auto createResult = TlsClient::Create("one.one.one.one", IPAddress::FromIPv4(TEST_SERVER_IP), TLS_PORT);
		if (!createResult)
		{
			LOG_ERROR("TLS client creation failed (error: %e)", createResult.Error());
			return false;
		}
		TlsClient &tls = createResult.Value();

		auto openResult = tls.Open();
		if (!openResult)
		{
			LOG_ERROR("TLS handshake failed (error: %e)", openResult.Error());
			return false;
		}
		LOG_INFO("  PASSED: TLS handshake");

		BOOL allPassed = true;

		// --- Single message echo ---
		{
			const CHAR msg[] = "GET / HTTP/1.1\r\n"
						"Host: one.one.one.one\r\n"
						"\r\n";
			if (SendAndVerifyHttp(tls, Span<const CHAR>(msg, sizeof(msg) - 1), "single message"))
				LOG_INFO("  PASSED: TLS echo - single message");
			else
			{
				LOG_ERROR("  FAILED: TLS echo - single message");
				allPassed = false;
			}
		}

		// --- Multiple sequential messages on same connection ---
		{
			const CHAR msg2[] = "GET / HTTP/1.1\r\n"
						 "Host: one.one.one.one\r\n"
						 "\r\n";
			if (SendAndVerifyHttp(tls, Span<const CHAR>(msg2, sizeof(msg2) - 1), "message 2"))
				LOG_INFO("  PASSED: TLS echo - message 2");
			else
			{
				LOG_ERROR("  FAILED: TLS echo - message 2");
				allPassed = false;
			}

			const CHAR msg3[] = "GET / HTTP/1.1\r\n"
						 "Host: one.one.one.one\r\n"
						 "Connection: close\r\n"
						 "\r\n";
			if (SendAndVerifyHttp(tls, Span<const CHAR>(msg3, sizeof(msg3) - 1), "message 3"))
				LOG_INFO("  PASSED: TLS echo - message 3");
			else
			{
				LOG_ERROR("  FAILED: TLS echo - message 3");
				allPassed = false;
			}
		}

		(VOID)tls.Close();
		return allPassed;
	}

public:
	// Run all TLS tests
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running TLS Tests...");
		LOG_INFO("  Test Server: one.one.one.one (1.1.1.1:443)");

		if (!TestTlsSuite())
			allPassed = false;

		if (allPassed)
			LOG_INFO("All TLS tests passed!");
		else
			LOG_ERROR("Some TLS tests failed!");

		return allPassed;
	}
};
