#pragma once

#include "lib/runtime.h"
#include "platform/system/pipe.h"
#include "platform/system/process.h"
#include "tests.h"

class PipeTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Pipe Tests...");

#if defined(PLATFORM_UEFI)
		RunTest(allPassed, &TestCreateFailsOnUefi, "Create fails on UEFI");
#else
		RunTest(allPassed, &TestCreate, "Create pipe");
		RunTest(allPassed, &TestReadWrite, "Write and read through pipe");
		RunTest(allPassed, &TestCloseEnds, "Close individual ends");
		RunTest(allPassed, &TestMoveSemantics, "Move constructor and assignment");
		RunTest(allPassed, &TestCaptureChildStdout, "Capture child process stdout");
#endif

		if (allPassed)
			LOG_INFO("All Pipe tests passed!");
		else
			LOG_ERROR("Some Pipe tests failed!");

		return allPassed;
	}

private:
#if defined(PLATFORM_UEFI)
	static BOOL TestCreateFailsOnUefi()
	{
		auto result = Pipe::Create();
		if (result)
		{
			LOG_ERROR("Pipe::Create should fail on UEFI");
			return false;
		}
		return true;
	}
#else
	static BOOL TestCreate()
	{
		auto result = Pipe::Create();
		if (!result)
		{
			LOG_ERROR("Pipe::Create failed: %e", result.Error());
			return false;
		}

		auto &pipe = result.Value();
		if (!pipe.IsValid())
		{
			LOG_ERROR("Pipe should be valid after Create");
			return false;
		}

		if (pipe.ReadEnd() == -1 || pipe.WriteEnd() == -1)
		{
			LOG_ERROR("Both pipe ends should be valid");
			return false;
		}

		return true;
	}

	static BOOL TestReadWrite()
	{
		auto result = Pipe::Create();
		if (!result)
		{
			LOG_ERROR("Pipe::Create failed: %e", result.Error());
			return false;
		}

		auto &pipe = result.Value();

		// Write data
		auto msg = "hello pipe";
		USIZE msgLen = StringUtils::Length((const CHAR *)msg);
		auto writeResult = pipe.Write(Span<const UINT8>((const UINT8 *)(const CHAR *)msg, msgLen));
		if (!writeResult)
		{
			LOG_ERROR("Pipe::Write failed: %e", writeResult.Error());
			return false;
		}

		if (writeResult.Value() != msgLen)
		{
			LOG_ERROR("Expected to write %u bytes, wrote %u", (UINT32)msgLen, (UINT32)writeResult.Value());
			return false;
		}

		// Close write end so read gets EOF after data
		(VOID)pipe.CloseWrite();

		// Read data
		UINT8 buf[64]{};
		auto readResult = pipe.Read(Span<UINT8>(buf, sizeof(buf)));
		if (!readResult)
		{
			LOG_ERROR("Pipe::Read failed: %e", readResult.Error());
			return false;
		}

		if (readResult.Value() != msgLen)
		{
			LOG_ERROR("Expected to read %u bytes, read %u", (UINT32)msgLen, (UINT32)readResult.Value());
			return false;
		}

		// Verify content
		if (Memory::Compare(buf, (const CHAR *)msg, msgLen) != 0)
		{
			LOG_ERROR("Read data does not match written data");
			return false;
		}

		return true;
	}

	static BOOL TestCloseEnds()
	{
		auto result = Pipe::Create();
		if (!result)
		{
			LOG_ERROR("Pipe::Create failed: %e", result.Error());
			return false;
		}

		auto &pipe = result.Value();

		// Close read end
		(VOID)pipe.CloseRead();
		if (pipe.ReadEnd() != -1)
		{
			LOG_ERROR("ReadEnd should be -1 after CloseRead");
			return false;
		}

		// Close write end
		(VOID)pipe.CloseWrite();
		if (pipe.WriteEnd() != -1)
		{
			LOG_ERROR("WriteEnd should be -1 after CloseWrite");
			return false;
		}

		// Pipe should be invalid now
		if (pipe.IsValid())
		{
			LOG_ERROR("Pipe should be invalid after closing both ends");
			return false;
		}

		return true;
	}

	static BOOL TestMoveSemantics()
	{
		auto result = Pipe::Create();
		if (!result)
		{
			LOG_ERROR("Pipe::Create failed: %e", result.Error());
			return false;
		}

		// Move construct
		Pipe moved(static_cast<Pipe &&>(result.Value()));
		if (!moved.IsValid())
		{
			LOG_ERROR("Moved pipe should be valid");
			return false;
		}

		// Original should be invalid after move
		if (result.Value().IsValid())
		{
			LOG_ERROR("Source pipe should be invalid after move");
			return false;
		}

		return true;
	}

	static BOOL TestCaptureChildStdout()
	{
		// Create a pipe to capture child's stdout
		auto pipeResult = Pipe::Create();
		if (!pipeResult)
		{
			LOG_ERROR("Pipe::Create failed: %e", pipeResult.Error());
			return false;
		}

		auto &pipe = pipeResult.Value();

		// Spawn child that writes to stdout
#if defined(PLATFORM_WINDOWS)
		auto cmd = "C:\\Windows\\System32\\cmd.exe";
		auto a1 = "/c";
		auto a2 = "echo";
		auto a3 = "hello";
		const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, (const CHAR *)a3, nullptr};
#else
		auto cmd = "/bin/sh";
		auto a1 = "-c";
		auto a2 = "echo hello";
		const CHAR *args[] = {(const CHAR *)cmd, (const CHAR *)a1, (const CHAR *)a2, nullptr};
#endif

		auto procResult = Process::Create(
			(const CHAR *)cmd, args,
			-1,              // inherit stdin
			pipe.WriteEnd(), // redirect stdout to pipe write end
			-1);             // inherit stderr

		if (!procResult)
		{
			LOG_ERROR("Process::Create failed: %e", procResult.Error());
			return false;
		}

		// Close write end in parent — child has its own copy
		(VOID)pipe.CloseWrite();

		// Wait for child to exit
		auto &proc = procResult.Value();
		auto waitResult = proc.Wait();
		if (!waitResult)
		{
			LOG_ERROR("Wait failed: %e", waitResult.Error());
			return false;
		}

		// Read captured output
		UINT8 buf[256]{};
		auto readResult = pipe.Read(Span<UINT8>(buf, sizeof(buf)));
		if (!readResult)
		{
			LOG_ERROR("Pipe::Read failed: %e", readResult.Error());
			return false;
		}

		USIZE bytesRead = readResult.Value();
		if (bytesRead == 0)
		{
			LOG_ERROR("Expected to read output from child, got 0 bytes");
			return false;
		}

		// Verify the output contains "hello"
		auto expected = "hello";
		BOOL found = false;
		USIZE expectedLen = StringUtils::Length((const CHAR *)expected);
		for (USIZE i = 0; i + expectedLen <= bytesRead; ++i)
		{
			if (Memory::Compare(buf + i, (const CHAR *)expected, expectedLen) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			LOG_ERROR("Child output does not contain 'hello'");
			return false;
		}

		LOG_INFO("Captured child output (%u bytes): contains 'hello'", (UINT32)bytesRead);
		return true;
	}
#endif // !PLATFORM_UEFI
};
