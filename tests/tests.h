/**
 * Tests.h - Test Utilities
 *
 * Provides template functions for running tests with pass/fail logging.
 * Uses function-based API for better IDE support and code highlighting.
 */

#pragma once

#include "platform/console/logger.h"
#include "core/types/span.h"

/**
 * RunTest - Run a test function with pass/fail logging
 *
 * Executes a test function and logs the result. Updates the allPassed variable
 * if the test fails.
 *
 * USAGE:
 *   BOOL allPassed = true;
 *   RunTest(allPassed, TestFunction, "Test description");
 *
 * @param allPassedVar - Boolean variable to track overall test status
 * @param testFunc     - Test function to execute (must return BOOL)
 * @param description  - Human-readable description of the test (embedded string)
 * @return true if test passed, false otherwise
 */
#if defined(ENABLE_LOGGING)
template <typename TestFunc>
NOINLINE BOOL RunTest(BOOL &allPassedVar, TestFunc testFunc, PCCHAR description)
{
	BOOL passed = testFunc();
	if (!passed)
	{
		allPassedVar = false;
		LOG_ERROR("  FAILED: %s", description);
	}
	else
	{
		LOG_INFO("  PASSED: %s", description);
	}
	asm volatile("" ::: "memory");
	return passed;
}
#else
template <typename TestFunc>
inline BOOL RunTest(BOOL &allPassedVar, TestFunc testFunc)
{
	if (!testFunc())
	{
		allPassedVar = false;
		return false;
	}
	return true;
}
// Strip the description argument at the call site to prevent embedded string construction
#define RunTest(allPassed, func, ...) RunTest(allPassed, func)
#endif

/**
 * RunTestSuite - Run a test suite's RunAll() method
 *
 * Executes a test suite's RunAll() method and updates the overall status.
 * Adds a blank line after the suite for visual separation.
 *
 * USAGE:
 *   BOOL allPassed = true;
 *   RunTestSuite<MemoryTests>(allPassed);
 *   RunTestSuite<StringTests>(allPassed);
 *
 * @param allPassedVar - Boolean variable to track overall test status
 * @return true if all tests in suite passed, false otherwise
 */
template <typename TestSuite>
inline BOOL RunTestSuite(BOOL &allPassedVar)
{
	BOOL result = TestSuite::RunAll();
	if (!result)
		allPassedVar = false;
	LOG_INFO("");
	return result;
}

/**
 * CompareBytes - Compare two byte arrays for equality
 */
inline BOOL CompareBytes(Span<const UINT8> a, Span<const UINT8> b)
{
	if (a.Size() != b.Size())
		return false;
	for (USIZE i = 0; i < a.Size(); i++)
	{
		if (a[i] != b[i])
			return false;
	}
	return true;
}

/**
 * IsAllZeros - Check if all bytes in a buffer are zero
 */
inline BOOL IsAllZeros(Span<const UINT8> data)
{
	for (USIZE i = 0; i < data.Size(); i++)
	{
		if (data[i] != 0)
			return false;
	}
	return true;
}

/**
 * Median - In-place median of sample counts (insertion sort; mutates samples)
 */
inline UINT64 Median(UINT64 *samples, UINT32 count)
{
	for (UINT32 i = 1; i < count; i++)
	{
		UINT64 key = samples[i];
		INT32 j = (INT32)i - 1;
		while (j >= 0 && samples[j] > key)
		{
			samples[j + 1] = samples[j];
			j--;
		}
		samples[j + 1] = key;
	}
	return samples[count / 2];
}
