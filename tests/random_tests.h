#pragma once

#include "lib/runtime.h"
#include "platform/system/random.h"
#include "tests.h"

class RandomTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Random Tests...");

		// Test 1: Basic instantiation
		LOG_INFO("  Creating Random object...");
		Random rng;
		(VOID)rng;
		LOG_INFO("  Random object created!");

		RunTest(allPassed, &TestGenerationSuite, "Generation suite");
		RunTest(allPassed, &TestStringCharSuite, "String/Char suite");

		if (allPassed)
			LOG_INFO("All Random tests passed!");
		else
			LOG_ERROR("Some Random tests failed!");

		return allPassed;
	}

private:
	static BOOL TestGenerationSuite()
	{
		BOOL allPassed = true;

		// --- Basic generation ---
		{
			Random rng;

			// Generate a few random numbers and verify they're generated
			// We just verify the calls succeed without checking specific values
			rng.Get();
			rng.Get();
			rng.Get();

			// Just verify we got values (not checking for specific values due to randomness)
			// We'll check range in another test
			LOG_INFO("  PASSED: Basic random number generation");
		}

		// --- Value range ---
		{
			Random rng;
			BOOL passed = true;

			// Test 100 random values to ensure they're all within range [0, Random::Max]
			for (INT32 i = 0; i < 100; i++)
			{
				INT32 val = rng.Get();
				if (val < 0 || val >= Random::Max)
				{
					LOG_ERROR("Random value out of range: %d (max: %d)", val, Random::Max);
					passed = false;
					break;
				}
			}

			if (passed)
				LOG_INFO("  PASSED: Random values within range");
			else
			{
				LOG_ERROR("  FAILED: Random values within range");
				allPassed = false;
			}
		}

		// --- Sequence variability ---
		{
			Random rng;

			// Generate 20 values and verify they're not all the same
			INT32 values[20];
			for (INT32 i = 0; i < 20; i++)
			{
				values[i] = rng.Get();
			}

			// Check that at least some values differ
			BOOL foundDifferent = false;
			for (INT32 i = 1; i < 20; i++)
			{
				if (values[i] != values[0])
				{
					foundDifferent = true;
					break;
				}
			}

			if (foundDifferent)
				LOG_INFO("  PASSED: Random sequence variability");
			else
			{
				LOG_ERROR("All 20 random values are identical: %d", values[0]);
				LOG_ERROR("  FAILED: Random sequence variability");
				allPassed = false;
			}
		}

		// --- Byte array generation ---
		{
			Random rng;
			UINT8 buffer[64];

			// Initialize buffer to known value
			Memory::Zero(buffer, sizeof(buffer));

			// Fill buffer with random bytes
			rng.GetArray(Span<UINT8>(buffer, 64));

			// Verify at least some bytes are non-zero (very unlikely all would be zero)
			BOOL foundNonZero = false;
			for (USIZE i = 0; i < 64; i++)
			{
				if (buffer[i] != 0)
				{
					foundNonZero = true;
					break;
				}
			}

			if (foundNonZero)
				LOG_INFO("  PASSED: Random byte array generation");
			else
			{
				LOG_ERROR("All 64 random bytes are zero");
				LOG_ERROR("  FAILED: Random byte array generation");
				allPassed = false;
			}
		}

		return allPassed;
	}

	static BOOL TestStringCharSuite()
	{
		BOOL allPassed = true;

		// --- Char generation ---
		{
			Random rng;
			BOOL passed = true;

			// Test narrow char generation
			for (INT32 i = 0; i < 50; i++)
			{
				CHAR c = rng.GetChar<CHAR>();
				// Verify character is lowercase a-z
				if (c < 'a' || c > 'z')
				{
					LOG_ERROR("Narrow char out of range: 0x%02X", (UINT32)(UINT8)c);
					passed = false;
					break;
				}
			}

			// Test wide char generation
			if (passed)
			{
				for (INT32 i = 0; i < 50; i++)
				{
					WCHAR c = rng.GetChar<WCHAR>();
					// Verify character is lowercase a-z
					if (c < L'a' || c > L'z')
					{
						LOG_ERROR("Wide char out of range: 0x%04X", (UINT32)c);
						passed = false;
						break;
					}
				}
			}

			if (passed)
				LOG_INFO("  PASSED: Random character generation");
			else
			{
				LOG_ERROR("  FAILED: Random character generation");
				allPassed = false;
			}
		}

		// --- String generation (narrow) ---
		{
			Random rng;
			CHAR buffer[32];
			BOOL passed = true;

			// Generate string of 10 random chars (span size = 11 to include null terminator)
			UINT32 len = rng.GetString<CHAR>(Span<CHAR>(buffer, 11));

			// Verify length
			if (len != 10)
			{
				LOG_ERROR("Narrow string length: expected 10, got %u", len);
				passed = false;
			}

			// Verify null termination
			if (passed && buffer[10] != '\0')
			{
				LOG_ERROR("Narrow string not null-terminated at position 10");
				passed = false;
			}

			// Verify all characters are lowercase letters
			if (passed)
			{
				for (UINT32 i = 0; i < len; i++)
				{
					if (buffer[i] < 'a' || buffer[i] > 'z')
					{
						LOG_ERROR("Narrow string char[%u] out of range: 0x%02X", i, (UINT32)(UINT8)buffer[i]);
						passed = false;
						break;
					}
				}
			}

			if (passed)
				LOG_INFO("  PASSED: Random string generation (narrow)");
			else
			{
				LOG_ERROR("  FAILED: Random string generation (narrow)");
				allPassed = false;
			}
		}

		// --- String generation (wide) ---
		{
			Random rng;
			WCHAR buffer[32];
			BOOL passed = true;

			// Generate string of 15 random chars (span size = 16 to include null terminator)
			UINT32 len = rng.GetString<WCHAR>(Span<WCHAR>(buffer, 16));

			// Verify length
			if (len != 15)
			{
				LOG_ERROR("Wide string length: expected 15, got %u", len);
				passed = false;
			}

			// Verify null termination
			if (passed && buffer[15] != L'\0')
			{
				LOG_ERROR("Wide string not null-terminated at position 15");
				passed = false;
			}

			// Verify all characters are lowercase letters
			if (passed)
			{
				for (UINT32 i = 0; i < len; i++)
				{
					if (buffer[i] < L'a' || buffer[i] > L'z')
					{
						LOG_ERROR("Wide string char[%u] out of range: 0x%04X", i, (UINT32)buffer[i]);
						passed = false;
						break;
					}
				}
			}

			if (passed)
				LOG_INFO("  PASSED: Random string generation (wide)");
			else
			{
				LOG_ERROR("  FAILED: Random string generation (wide)");
				allPassed = false;
			}
		}

		// --- Empty string ---
		{
			Random rng;
			CHAR buffer[16];
			BOOL passed = true;

			// Generate empty string (span size = 1 for null terminator only)
			UINT32 len = rng.GetString<CHAR>(Span<CHAR>(buffer, 1));

			// Verify length is 0
			if (len != 0)
			{
				LOG_ERROR("Empty string length: expected 0, got %u", len);
				passed = false;
			}

			// Verify null termination at position 0
			if (passed && buffer[0] != '\0')
			{
				LOG_ERROR("Empty string not null-terminated at position 0");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Empty string generation");
			else
			{
				LOG_ERROR("  FAILED: Empty string generation");
				allPassed = false;
			}
		}

		return allPassed;
	}
};
