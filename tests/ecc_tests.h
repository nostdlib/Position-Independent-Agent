#pragma once

#include "lib/runtime.h"
#include "tests.h"

class EccTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running ECC Tests...");

		RunTest(allPassed, &TestCurveSuite, "ECC curve suite");
		RunTest(allPassed, &TestECDHSuite, "ECDH suite");
		RunTest(allPassed, &TestErrorHandlingSuite, "ECC error handling suite");

		if (allPassed)
			LOG_INFO("All ECC tests passed!");
		else
			LOG_ERROR("Some ECC tests failed!");

		return allPassed;
	}

private:
	// Curve Suite: initialization, secp256r1, secp384r1, public key export, format
	static BOOL TestCurveSuite()
	{
		BOOL allPassed = true;

		// Test: Basic ECC initialization
		{
			ECC ecc;
			auto result = ecc.Initialize(32);
			if (result)
				LOG_INFO("  PASSED: ECC initialization");
			else
			{
				LOG_ERROR("  FAILED: ECC Initialize(32) failed (error: %e)", result.Error());
				allPassed = false;
			}
		}

		// Test: secp256r1 curve (32 bytes)
		{
			ECC ecc;
			auto initResult = ecc.Initialize(32);
			if (!initResult)
			{
				LOG_ERROR("  FAILED: secp256r1 Initialize failed (error: %e)", initResult.Error());
				allPassed = false;
			}
			else
			{
				UINT8 publicKey[32 * 2 + 1];
				auto result = ecc.ExportPublicKey(Span<UINT8>(publicKey));
				if (!result)
				{
					LOG_ERROR("  FAILED: secp256r1 ExportPublicKey failed (error: %e)", result.Error());
					allPassed = false;
				}
				else if (publicKey[0] != 0x04)
				{
					LOG_ERROR("  FAILED: secp256r1 public key format byte: expected 0x04, got 0x%02X", (UINT32)publicKey[0]);
					allPassed = false;
				}
				else
					LOG_INFO("  PASSED: ECC secp256r1");
			}
		}

		// Test: secp384r1 curve (48 bytes)
		{
			ECC ecc;
			auto initResult = ecc.Initialize(48);
			if (!initResult)
			{
				LOG_ERROR("  FAILED: secp384r1 Initialize failed (error: %e)", initResult.Error());
				allPassed = false;
			}
			else
			{
				UINT8 publicKey[48 * 2 + 1];
				auto result = ecc.ExportPublicKey(Span<UINT8>(publicKey));
				if (!result)
				{
					LOG_ERROR("  FAILED: secp384r1 ExportPublicKey failed (error: %e)", result.Error());
					allPassed = false;
				}
				else if (publicKey[0] != 0x04)
				{
					LOG_ERROR("  FAILED: secp384r1 public key format byte: expected 0x04, got 0x%02X", (UINT32)publicKey[0]);
					allPassed = false;
				}
				else
					LOG_INFO("  PASSED: ECC secp384r1");
			}
		}

		// Test: Public key export functionality
		{
			ECC ecc;
			(VOID)ecc.Initialize(32);

			UINT8 publicKey[32 * 2 + 1];
			auto result = ecc.ExportPublicKey(Span<UINT8>(publicKey));

			if (!result)
			{
				LOG_ERROR("  FAILED: Public key export failed (error: %e)", result.Error());
				allPassed = false;
			}
			else if (IsAllZeros(Span<const UINT8>(publicKey)))
			{
				LOG_ERROR("  FAILED: Public key is all zeros");
				allPassed = false;
			}
			else
				LOG_INFO("  PASSED: Public key export");
		}

		// Test: Public key format validation
		{
			ECC ecc;
			(VOID)ecc.Initialize(32);

			UINT8 publicKey[32 * 2 + 1];
			(VOID)ecc.ExportPublicKey(Span<UINT8>(publicKey));

			if (publicKey[0] != 0x04)
			{
				LOG_ERROR("  FAILED: Public key format byte: expected 0x04, got 0x%02X", (UINT32)publicKey[0]);
				allPassed = false;
			}
			else
			{
				BOOL xAllZeros = IsAllZeros(Span<const UINT8>(publicKey + 1, 32));
				BOOL yAllZeros = IsAllZeros(Span<const UINT8>(publicKey + 1 + 32, 32));

				if (xAllZeros && yAllZeros)
				{
					LOG_ERROR("  FAILED: Both X and Y coordinates are all zeros");
					allPassed = false;
				}
				else
					LOG_INFO("  PASSED: Public key format");
			}
		}

		return allPassed;
	}

	// ECDH Suite: shared secret computation, multiple key generation uniqueness
	static BOOL TestECDHSuite()
	{
		BOOL allPassed = true;

		// Test: Shared secret computation (ECDH key exchange)
		{
			ECC alice, bob;

			(VOID)alice.Initialize(32);
			(VOID)bob.Initialize(32);

			UINT8 alicePublicKey[32 * 2 + 1];
			UINT8 bobPublicKey[32 * 2 + 1];

			(VOID)alice.ExportPublicKey(Span<UINT8>(alicePublicKey));
			(VOID)bob.ExportPublicKey(Span<UINT8>(bobPublicKey));

			UINT8 aliceSecret[32];
			UINT8 bobSecret[32];

			auto aliceResult = alice.ComputeSharedSecret(Span<const UINT8>(bobPublicKey), Span<UINT8>(aliceSecret));
			if (!aliceResult)
			{
				LOG_ERROR("  FAILED: Alice ECDH shared secret computation failed (error: %e)", aliceResult.Error());
				allPassed = false;
			}
			else
			{
				auto bobResult = bob.ComputeSharedSecret(Span<const UINT8>(alicePublicKey), Span<UINT8>(bobSecret));
				if (!bobResult)
				{
					LOG_ERROR("  FAILED: Bob ECDH shared secret computation failed (error: %e)", bobResult.Error());
					allPassed = false;
				}
				else if (!CompareBytes(Span<const UINT8>(aliceSecret), Span<const UINT8>(bobSecret)))
				{
					LOG_ERROR("  FAILED: ECDH shared secrets do not match");
					allPassed = false;
				}
				else
					LOG_INFO("  PASSED: Shared secret computation (ECDH)");
			}
		}

		// Test: Sequential key generation produces different keys
		{
			ECC ecc1;
			(VOID)ecc1.Initialize(32);

			UINT8 pubKey1[32 * 2 + 1];
			(VOID)ecc1.ExportPublicKey(Span<UINT8>(pubKey1));

			ECC ecc2;
			(VOID)ecc2.Initialize(32);

			UINT8 pubKey2[32 * 2 + 1];
			(VOID)ecc2.ExportPublicKey(Span<UINT8>(pubKey2));

			BOOL key1DiffersFrom2 = !CompareBytes(Span<const UINT8>(pubKey1), Span<const UINT8>(pubKey2));
			BOOL key1Valid = pubKey1[0] == 0x04 && !IsAllZeros(Span<const UINT8>(pubKey1 + 1, 64));
			BOOL key2Valid = pubKey2[0] == 0x04 && !IsAllZeros(Span<const UINT8>(pubKey2 + 1, 64));

			if (!key1DiffersFrom2 || !key1Valid || !key2Valid)
			{
				LOG_ERROR("  FAILED: Key generation uniqueness: key1Valid=%d, key2Valid=%d, differ=%d", key1Valid, key2Valid, key1DiffersFrom2);
				allPassed = false;
			}
			else
				LOG_INFO("  PASSED: Multiple key generation uniqueness");
		}

		return allPassed;
	}

	// Error Handling Suite: invalid curve size, buffer validation, invalid public key
	static BOOL TestErrorHandlingSuite()
	{
		BOOL allPassed = true;

		// Test: Invalid curve size handling
		{
			ECC ecc;
			auto result = ecc.Initialize(64);

			if (result.IsErr())
				LOG_INFO("  PASSED: Invalid curve size handling");
			else
			{
				LOG_ERROR("  FAILED: Initialize(64) should have failed but succeeded");
				allPassed = false;
			}
		}

		// Test: Export buffer size validation
		{
			ECC ecc;
			(VOID)ecc.Initialize(32);

			UINT8 tooSmallBuffer[32];
			auto result = ecc.ExportPublicKey(Span<UINT8>(tooSmallBuffer));

			if (result.IsErr())
				LOG_INFO("  PASSED: Export buffer size validation");
			else
			{
				LOG_ERROR("  FAILED: ExportPublicKey with small buffer should have failed but succeeded");
				allPassed = false;
			}
		}

		// Test: Invalid public key handling
		{
			ECC ecc;
			(VOID)ecc.Initialize(32);

			UINT8 invalidPublicKey[32 * 2 + 1];
			Memory::Zero(invalidPublicKey, sizeof(invalidPublicKey));
			invalidPublicKey[0] = 0x03;

			UINT8 secret[32];
			auto result = ecc.ComputeSharedSecret(Span<const UINT8>(invalidPublicKey), Span<UINT8>(secret));

			if (result.IsErr())
				LOG_INFO("  PASSED: Invalid public key handling");
			else
			{
				LOG_ERROR("  FAILED: ComputeSharedSecret with invalid key should have failed but succeeded");
				allPassed = false;
			}
		}

		return allPassed;
	}
};
