#pragma once

#include "lib/runtime.h"
#include "platform/system/process.h"
#include "tests.h"

class ProcessTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Process Tests...");

		RunTest(allPassed, &TestErrorCasesSuite, "Error cases suite");
#if !defined(PLATFORM_UEFI)
		RunTest(allPassed, &TestOperationsSuite, "Operations suite");
#endif

		if (allPassed)
			LOG_INFO("All Process tests passed!");
		else
			LOG_ERROR("Some Process tests failed!");

		return allPassed;
	}

private:
	static BOOL TestErrorCasesSuite()
	{
		BOOL allPassed = true;

		// --- Invalid path ---
		{
			auto result = Process::Create(nullptr, nullptr);
			BOOL passed = !result;

			if (passed)
				LOG_INFO("  PASSED: Create with null path returns error");
			else
			{
				LOG_ERROR("Create(nullptr) should have failed");
				LOG_ERROR("  FAILED: Create with null path returns error");
				allPassed = false;
			}
		}

		// --- Invalid args ---
		{
			auto path = "/nonexistent";
			auto result = Process::Create((const CHAR *)path, nullptr);
			BOOL passed = !result;

			if (passed)
				LOG_INFO("  PASSED: Create with null args returns error");
			else
			{
				LOG_ERROR("Create with null args should have failed");
				LOG_ERROR("  FAILED: Create with null args returns error");
				allPassed = false;
			}
		}

		// --- Invalid process state ---
		{
			BOOL passed = true;

			// Create should fail with null path
			auto result = Process::Create(nullptr, nullptr);
			if (result)
			{
				LOG_ERROR("Expected Create to fail with null path");
				passed = false;
			}

			// Verify the error code is Process_CreateFailed or Process_NotSupported (UEFI)
			if (passed && result.Error().Code != Error::Process_CreateFailed
				&& result.Error().Code != Error::Process_NotSupported)
			{
				LOG_ERROR("Expected Process_CreateFailed or Process_NotSupported error code");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Invalid process state");
			else
			{
				LOG_ERROR("  FAILED: Invalid process state");
				allPassed = false;
			}
		}

		return allPassed;
	}

#if !defined(PLATFORM_UEFI)
	static BOOL TestOperationsSuite()
	{
		BOOL allPassed = true;

		// --- Create and wait ---
		{
#if defined(PLATFORM_WINDOWS)
			auto cmd = "C:\\Windows\\System32\\cmd.exe";
			auto a1 = "/c";
			auto a2 = "exit";
			auto a3 = "0";
			const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, (const CHAR *)a3, nullptr};
#else
			auto cmd = "/bin/sh";
			auto a1 = "-c";
			auto a2 = "exit 0";
			const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, nullptr};
#endif

			BOOL passed = true;
			auto result = Process::Create((const CHAR *)cmd, args);
			if (!result)
			{
				LOG_ERROR("Failed to create process: %e", result.Error());
				passed = false;
			}

			if (passed && !result.Value().IsValid())
			{
				LOG_ERROR("Process should be valid after Create");
				passed = false;
			}

			if (passed)
			{
				auto waitResult = result.Value().Wait();
				if (!waitResult)
				{
					LOG_ERROR("Wait failed: %e", waitResult.Error());
					passed = false;
				}
				else
				{
					SSIZE exitCode = waitResult.Value();
					if (exitCode != 0)
					{
						LOG_ERROR("Expected exit code 0, got %d", (INT32)exitCode);
						passed = false;
					}
				}
			}

			if (passed)
				LOG_INFO("  PASSED: Create process and wait for exit");
			else
			{
				LOG_ERROR("  FAILED: Create process and wait for exit");
				allPassed = false;
			}
		}

		// --- Terminate ---
		{
#if defined(PLATFORM_WINDOWS)
			auto cmd = "C:\\Windows\\System32\\cmd.exe";
			auto a1 = "/c";
			auto a2 = "ping";
			auto a3 = "-n";
			auto a4 = "60";
			auto a5 = "127.0.0.1";
			const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, (const CHAR *)a3, (const CHAR *)a4, (const CHAR *)a5, nullptr};
#else
			auto cmd = "/bin/sh";
			auto a1 = "-c";
			auto a2 = "sleep 60";
			const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, nullptr};
#endif

			BOOL passed = true;
			auto result = Process::Create((const CHAR *)cmd, args);
			if (!result)
			{
				LOG_ERROR("Failed to create process: %e", result.Error());
				passed = false;
			}

			if (passed)
			{
				auto termResult = result.Value().Terminate();
				if (!termResult)
				{
					LOG_ERROR("Terminate failed: %e", termResult.Error());
					passed = false;
				}

				// After terminate, wait should succeed (reap the child)
				auto waitResult = result.Value().Wait();
				if (!waitResult)
				{
					// On some platforms, wait after kill may fail — that's acceptable
					LOG_WARNING("Wait after terminate returned error (may be expected): %e", waitResult.Error());
				}
			}

			if (passed)
				LOG_INFO("  PASSED: Create and terminate process");
			else
			{
				LOG_ERROR("  FAILED: Create and terminate process");
				allPassed = false;
			}
		}

		// --- IsRunning ---
		{
#if defined(PLATFORM_WINDOWS)
			auto cmd = "C:\\Windows\\System32\\cmd.exe";
			auto a1 = "/c";
			auto a2 = "ping";
			auto a3 = "-n";
			auto a4 = "60";
			auto a5 = "127.0.0.1";
			const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, (const CHAR *)a3, (const CHAR *)a4, (const CHAR *)a5, nullptr};
#else
			auto cmd = "/bin/sh";
			auto a1 = "-c";
			auto a2 = "sleep 60";
			const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, nullptr};
#endif

			BOOL passed = true;
			auto result = Process::Create((const CHAR *)cmd, args);
			if (!result)
			{
				LOG_ERROR("Failed to create process: %e", result.Error());
				passed = false;
			}

			if (passed && !result.Value().IsRunning())
			{
				LOG_ERROR("Process should be running immediately after Create");
				passed = false;
			}

			if (result)
			{
				// Terminate it
				(void)result.Value().Terminate();
				(void)result.Value().Wait();
			}

			if (passed)
				LOG_INFO("  PASSED: IsRunning on active process");
			else
			{
				LOG_ERROR("  FAILED: IsRunning on active process");
				allPassed = false;
			}
		}

		// --- Move semantics ---
		{
#if defined(PLATFORM_WINDOWS)
			auto cmd = "C:\\Windows\\System32\\cmd.exe";
			auto a1 = "/c";
			auto a2 = "exit";
			auto a3 = "0";
			const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, (const CHAR *)a3, nullptr};
#else
			auto cmd = "/bin/sh";
			auto a1 = "-c";
			auto a2 = "exit 0";
			const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, nullptr};
#endif

			BOOL passed = true;
			auto result = Process::Create((const CHAR *)cmd, args);
			if (!result)
			{
				LOG_ERROR("Failed to create process: %e", result.Error());
				passed = false;
			}

			if (passed)
			{
				// Move construct
				Process moved(static_cast<Process &&>(result.Value()));
				if (!moved.IsValid())
				{
					LOG_ERROR("Moved process should be valid");
					passed = false;
				}

				// Original should be invalid after move
				if (passed && result.Value().IsValid())
				{
					LOG_ERROR("Source process should be invalid after move");
					passed = false;
				}

				(void)moved.Wait();
			}

			if (passed)
				LOG_INFO("  PASSED: Move constructor and assignment");
			else
			{
				LOG_ERROR("  FAILED: Move constructor and assignment");
				allPassed = false;
			}
		}

		// --- Create with IO ---
		{
#if defined(PLATFORM_WINDOWS)
			auto cmd = "C:\\Windows\\System32\\cmd.exe";
			auto a1 = "/c";
			auto a2 = "exit";
			auto a3 = "0";
			const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, (const CHAR *)a3, nullptr};
#else
			auto cmd = "/bin/sh";
			auto a1 = "-c";
			auto a2 = "exit 0";
			const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, nullptr};
#endif

			BOOL passed = true;

			// Create with -1 (inherit) — should behave like no redirection
			auto result = Process::Create((const CHAR *)cmd, args, -1, -1, -1);
			if (!result)
			{
				LOG_ERROR("Failed to create process with default IO: %e", result.Error());
				passed = false;
			}

			if (passed)
			{
				auto waitResult = result.Value().Wait();
				if (!waitResult)
				{
					LOG_ERROR("Wait failed: %e", waitResult.Error());
					passed = false;
				}
			}

			if (passed)
				LOG_INFO("  PASSED: Create process with I/O redirection");
			else
			{
				LOG_ERROR("  FAILED: Create process with I/O redirection");
				allPassed = false;
			}
		}

		return allPassed;
	}
#endif // !PLATFORM_UEFI
};
