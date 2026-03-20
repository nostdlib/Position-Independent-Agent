#pragma once

#include "lib/runtime.h"
#include "shell_process.h"
#include "tests.h"

class ShellProcessTests
{
public:
    static BOOL RunAll()
    {
        BOOL allPassed = true;

        LOG_INFO("Running shell process Tests...");

#if !defined(PLATFORM_UEFI)
        RunTest(allPassed, &TestShellProcessCreate, "Shell Process Create");
#else
        LOG_INFO("  SKIPPED: Shell Process Create (not supported on UEFI)");
#endif

        if (allPassed)
            LOG_INFO("All shell process tests passed!");
        else
            LOG_ERROR("Some shell process tests failed!");

        return allPassed;
    }

private:
    static BOOL TestShellProcessCreate()
    {
        auto createResult = ShellProcess::Create();
        if (!createResult)
        {
            LOG_ERROR("Shell creation failed (error: %e)", createResult.Error());
            return false;
        }

        LOG_INFO("  PASSED: Shell creation");
        return true;
    }
};
