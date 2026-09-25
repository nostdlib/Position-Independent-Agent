#pragma once

#include "lib/runtime.h"
#include "tests.h"

// =============================================================================
// WebSocket Tests - WebSocketClient Implementation Validation
// Test Server: echo.websocket.org - WebSocket Echo Service
//
// Tests are consolidated to use minimal connections to avoid server rate limits.
// =============================================================================

class WebSocketTests
{
private:
	// Helper: verify echo response matches sent data
	static BOOL VerifyEcho(WebSocketClient &ws, Span<const CHAR> sent, WebSocketOpcode expectedOpcode, [[maybe_unused]] PCCHAR label)
	{
		auto readResult = ws.Read();
		if (!readResult)
		{
			LOG_ERROR("Failed to receive echo for %s (error: %e)", label, readResult.Error());
			return false;
		}

		WebSocketMessage &response = readResult.Value();

		if (response.Opcode != expectedOpcode)
		{
			LOG_ERROR("%s: unexpected opcode: expected %d, got %d", label, (UINT8)expectedOpcode, (UINT8)response.Opcode);
			return false;
		}

		if (response.Length != sent.Size() || Memory::Compare(response.Data, sent.Data(), sent.Size()) != 0)
		{
			LOG_ERROR("%s: echo response does not match sent data", label);
			return false;
		}

		return true;
	}

	// Helper: send data and verify echo
	static BOOL SendAndVerifyEcho(WebSocketClient &ws, Span<const CHAR> data, WebSocketOpcode opcode, PCCHAR label)
	{
		auto writeResult = ws.Write(data, opcode);
		if (!writeResult)
		{
			LOG_ERROR("Failed to send %s (error: %e)", label, writeResult.Error());
			return false;
		}

		return VerifyEcho(ws, data, opcode, label);
	}

	// Helper: fill a buffer with a position-dependent pseudo-random pattern
	static VOID FillPattern(PCHAR buffer, UINT32 size)
	{
		for (UINT32 i = 0; i < size; i++)
			buffer[i] = (CHAR)((i * 31 + (i >> 8)) & 0xFF);
	}

	// Helper: WriteResponse must produce the same wire bytes as a manual
	// [status][corrId][body] splice, echoed back by the server
	static BOOL VerifySplicedEcho(WebSocketClient &ws, UINT32 status, UINT32 corrId, UINT32 bodySize, PCCHAR label)
	{
		PCHAR body = new CHAR[bodySize];
		PCHAR spliced = new CHAR[bodySize + 8];
		if (!body || !spliced)
		{
			LOG_ERROR("Failed to allocate memory for %s", label);
			delete[] body;
			delete[] spliced;
			return false;
		}

		FillPattern(body, bodySize);
		Memory::Copy(spliced, &status, sizeof(status));
		Memory::Copy(spliced + 4, &corrId, sizeof(corrId));
		Memory::Copy(spliced + 8, body, bodySize);

		BOOL ok = true;
		auto writeResult = ws.WriteResponse(status, corrId, Span<const CHAR>(body, bodySize), WebSocketOpcode::Binary);
		if (!writeResult)
			ok = false;
		else
		{
			auto readResult = ws.Read();
			if (!readResult || readResult.Value().Length != bodySize + 8 ||
			    Memory::Compare(readResult.Value().Data, spliced, bodySize + 8) != 0)
				ok = false;
		}

		if (ok)
			LOG_INFO("  PASSED: %s", label);
		else
			LOG_ERROR("  FAILED: %s", label);

		delete[] body;
		delete[] spliced;
		return ok;
	}

	// Connection 1: All echo-based tests on a single connection
	static BOOL TestEchoSuite()
	{
		LOG_INFO("Connecting to echo.websocket.org (wss://)...");

		const CHAR wssUrl[] = "wss://echo.websocket.org/";
		auto createResult = WebSocketClient::Create(Span<const CHAR>(wssUrl, sizeof(wssUrl) - 1));
		if (!createResult)
		{
			LOG_ERROR("WebSocket connection failed (error: %e)", createResult.Error());
			return false;
		}
		WebSocketClient &ws = createResult.Value();
		LOG_INFO("  PASSED: Secure WebSocket connection (wss://)");

		// Discard initial server message
		auto initialMsg = ws.Read();
		if (initialMsg)
			LOG_INFO("Received initial server message (%d bytes), discarding", initialMsg.Value().Length);

		BOOL allPassed = true;

		// --- Text echo ---
		{
			LOG_INFO("Test: WebSocket Text Echo");
			const CHAR msg[] = "Hello, WebSocket!";
			if (SendAndVerifyEcho(ws, Span<const CHAR>(msg, sizeof(msg) - 1), WebSocketOpcode::Text, "text echo"))
				LOG_INFO("  PASSED: WebSocket text echo");
			else
			{
				LOG_ERROR("  FAILED: WebSocket text echo");
				allPassed = false;
			}
		}

		// --- Binary echo ---
		{
			LOG_INFO("Test: WebSocket Binary Echo");
			UINT8 binaryData[11];
			for (UINT32 i = 0; i < 5; i++)
				binaryData[i] = (UINT8)(i + 1);
			for (UINT32 i = 5; i < 11; i++)
				binaryData[i] = (UINT8)(0xAA + ((i - 5) * 0x11));

			if (SendAndVerifyEcho(ws, Span<const CHAR>((PCHAR)binaryData, sizeof(binaryData)), WebSocketOpcode::Binary, "binary echo"))
				LOG_INFO("  PASSED: WebSocket binary echo");
			else
			{
				LOG_ERROR("  FAILED: WebSocket binary echo");
				allPassed = false;
			}
		}

		// --- Multiple sequential messages ---
		{
			LOG_INFO("Test: Multiple Sequential Messages");
			const CHAR msg1[] = "First message";
			const CHAR msg2[] = "Second message";
			const CHAR msg3[] = "Third message";

			BOOL seqPassed = true;
			seqPassed = SendAndVerifyEcho(ws, Span<const CHAR>(msg1, sizeof(msg1) - 1), WebSocketOpcode::Text, "message 1") && seqPassed;
			seqPassed = SendAndVerifyEcho(ws, Span<const CHAR>(msg2, sizeof(msg2) - 1), WebSocketOpcode::Text, "message 2") && seqPassed;
			seqPassed = SendAndVerifyEcho(ws, Span<const CHAR>(msg3, sizeof(msg3) - 1), WebSocketOpcode::Text, "message 3") && seqPassed;

			if (seqPassed)
				LOG_INFO("  PASSED: Multiple messages");
			else
			{
				LOG_ERROR("  FAILED: Multiple messages");
				allPassed = false;
			}
		}

		// --- Large message ---
		{
			LOG_INFO("Test: Large Message Handling");
			UINT32 largeMessageSize = 1024;
			PCHAR largeMessage = new CHAR[largeMessageSize];
			if (!largeMessage)
			{
				LOG_ERROR("Failed to allocate memory for large message");
				allPassed = false;
			}
			else
			{
				for (UINT32 i = 0; i < largeMessageSize; i++)
					largeMessage[i] = 'A' + (i % 26);

				if (SendAndVerifyEcho(ws, Span<const CHAR>(largeMessage, largeMessageSize), WebSocketOpcode::Text, "large message"))
					LOG_INFO("  PASSED: Large message");
				else
				{
					LOG_ERROR("  FAILED: Large message");
					allPassed = false;
				}

				delete[] largeMessage;
			}
		}

		// --- Frame length encoding boundaries: 7-bit max, 16-bit min/max, 64-bit min ---
		{
			LOG_INFO("Test: Frame Length Boundaries");
			const UINT32 sizes[4] = {125, 126, 65535, 65536};
			PCHAR buffer = new CHAR[65536];
			if (!buffer)
			{
				LOG_ERROR("Failed to allocate memory for boundary frames");
				allPassed = false;
			}
			else
			{
				BOOL boundariesPassed = true;
				for (UINT32 i = 0; i < 4; i++)
				{
					FillPattern(buffer, sizes[i]);
					if (!SendAndVerifyEcho(ws, Span<const CHAR>(buffer, sizes[i]), WebSocketOpcode::Binary, "boundary frame"))
						boundariesPassed = false;
				}

				if (boundariesPassed)
					LOG_INFO("  PASSED: Frame length boundaries");
				else
				{
					LOG_ERROR("  FAILED: Frame length boundaries");
					allPassed = false;
				}

				delete[] buffer;
			}
		}

		// --- Minimum payload and mask-phase tails (payload % 4 == 1..3) ---
		{
			LOG_INFO("Test: Payload Alignment Tails");
			const UINT32 sizes[3] = {1, 300 * 1024 + 1, 300 * 1024 + 3};
			PCHAR buffer = new CHAR[300 * 1024 + 3];
			if (!buffer)
			{
				LOG_ERROR("Failed to allocate memory for alignment frames");
				allPassed = false;
			}
			else
			{
				BOOL tailsPassed = true;
				for (UINT32 i = 0; i < 3; i++)
				{
					FillPattern(buffer, sizes[i]);
					if (!SendAndVerifyEcho(ws, Span<const CHAR>(buffer, sizes[i]), WebSocketOpcode::Binary, "alignment frame"))
						tailsPassed = false;
				}

				if (tailsPassed)
					LOG_INFO("  PASSED: Payload alignment tails");
				else
				{
					LOG_ERROR("  FAILED: Payload alignment tails");
					allPassed = false;
				}

				delete[] buffer;
			}
		}

		// --- Oversized frame: multi-TLS-record masked payload (size % 4 == 0) ---
		{
			LOG_INFO("Test: Large Frame Mask Continuity");
			UINT32 bigSize = 300 * 1024;
			PCHAR bigMessage = new CHAR[bigSize];
			if (!bigMessage)
			{
				LOG_ERROR("Failed to allocate memory for oversized frame");
				allPassed = false;
			}
			else
			{
				FillPattern(bigMessage, bigSize);

				if (SendAndVerifyEcho(ws, Span<const CHAR>(bigMessage, bigSize), WebSocketOpcode::Binary, "300KiB frame"))
					LOG_INFO("  PASSED: Large frame mask continuity");
				else
				{
					LOG_ERROR("  FAILED: Large frame mask continuity");
					allPassed = false;
				}

				delete[] bigMessage;
			}
		}

		// --- WriteResponse splice equivalence: [status][corrId][body] on the wire ---
		{
			LOG_INFO("Test: WriteResponse Splice Equivalence");
			const UINT32 status = 0x12345678;
			const UINT32 corrId = 0xDEADBEEF;

			// Status-only reply (8-byte payload, small-frame path, prefix only)
			{
				UINT8 expected[8];
				Memory::Copy(expected, &status, sizeof(status));
				Memory::Copy(expected + 4, &corrId, sizeof(corrId));

				BOOL ok = true;
				auto writeResult = ws.WriteResponse(status, corrId, Span<const CHAR>(), WebSocketOpcode::Binary);
				if (!writeResult)
					ok = false;
				else
				{
					auto readResult = ws.Read();
					if (!readResult || readResult.Value().Length != sizeof(expected) ||
					    Memory::Compare(readResult.Value().Data, expected, sizeof(expected)) != 0)
						ok = false;
				}

				if (ok)
					LOG_INFO("  PASSED: WriteResponse status-only reply");
				else
				{
					LOG_ERROR("  FAILED: WriteResponse status-only reply");
					allPassed = false;
				}
			}

			// Small body crossing the 7-bit length boundary (payload 8 + 118 = 126)
			// and a large multi-record body, both vs a manual splice
			if (!VerifySplicedEcho(ws, status, corrId, 126 - 8, "WriteResponse boundary-length splice") ||
			    !VerifySplicedEcho(ws, status, corrId, 70000, "WriteResponse large splice equivalence"))
				allPassed = false;
		}

		// --- Close handshake ---
		{
			LOG_INFO("Test: WebSocket Close Handshake");
			auto closeResult = ws.Close();
			if (closeResult)
				LOG_INFO("  PASSED: WebSocket close");
			else
			{
				LOG_ERROR("WebSocket close handshake failed (error: %e)", closeResult.Error());
				LOG_ERROR("  FAILED: WebSocket close");
				allPassed = false;
			}
		}

		return allPassed;
	}

public:
	// Run all WebSocket tests
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running WebSocket Tests...");
		LOG_INFO("  Test Server: echo.websocket.org (wss://)");

		if (!TestEchoSuite())
			allPassed = false;

		if (allPassed)
			LOG_INFO("All WebSocket tests passed!");
		else
			LOG_ERROR("Some WebSocket tests failed!");

		return allPassed;
	}
};
