/**
 * @file process.cc
 * @brief Windows process management implementation
 *
 * @details Process creation via CreateProcessW with optional I/O redirection
 * using STARTUPINFOW. Lifecycle management via ZwWaitForSingleObject,
 * ZwTerminateProcess, and ZwClose.
 */

#include "platform/system/process.h"
#include "platform/kernel/windows/ntdll.h"
#include "platform/kernel/windows/kernel32.h"
#include "platform/kernel/windows/peb.h"

constexpr UINT32 STATUS_TIMEOUT = 0x00000102;

// ============================================================================
// Process::Create
// ============================================================================

Result<Process, Error> Process::Create(
	const CHAR *path,
	const CHAR *const args[],
	SSIZE stdinFd,
	SSIZE stdoutFd,
	SSIZE stderrFd) noexcept
{
	if (path == nullptr)
		return Result<Process, Error>::Err(Error::Process_CreateFailed);

	BOOL hasRedirect = (stdinFd != -1 || stdoutFd != -1 || stderrFd != -1);

	STARTUPINFOW si = {};
	si.cb = sizeof(si);

	if (hasRedirect)
	{
		si.dwFlags = STARTF_USESTDHANDLES;

		// For any fd that is -1, get the parent's handle from PEB
		PPEB peb = GetCurrentPEB();
		PRTL_USER_PROCESS_PARAMETERS params = peb->ProcessParameters;

		si.hStdInput = (stdinFd != -1)
			? (PVOID)(USIZE)stdinFd
			: params->StandardInput;
		si.hStdOutput = (stdoutFd != -1)
			? (PVOID)(USIZE)stdoutFd
			: params->StandardOutput;
		si.hStdError = (stderrFd != -1)
			? (PVOID)(USIZE)stderrFd
			: params->StandardError;

		// Make redirected handles inheritable
		if (stdinFd != -1)
			(VOID)Kernel32::SetHandleInformation(si.hStdInput, 1, 1);
		if (stdoutFd != -1)
			(VOID)Kernel32::SetHandleInformation(si.hStdOutput, 1, 1);
		if (stderrFd != -1)
			(VOID)Kernel32::SetHandleInformation(si.hStdError, 1, 1);
	}

	// Build command line: concatenate args with spaces into a wide string
	// Windows CreateProcessW expects a single command line string
	WCHAR cmdWide[1024]{};
	USIZE cmdPos = 0;

	if (args != nullptr)
	{
		for (USIZE i = 0; args[i] != nullptr; ++i)
		{
			if (i > 0 && cmdPos < 1023)
				cmdWide[cmdPos++] = L' ';

			USIZE argLen = StringUtils::Length(args[i]);
			USIZE converted = StringUtils::Utf8ToWide(
				Span<const CHAR>(args[i], argLen),
				Span<WCHAR>(cmdWide + cmdPos, 1024 - cmdPos));
			cmdPos += converted;
		}
	}
	else
	{
		// No args — use path as the command line
		StringUtils::Utf8ToWide(
			Span<const CHAR>(path, StringUtils::Length(path)),
			Span<WCHAR>(cmdWide, 1024));
	}

	PROCESS_INFORMATION pi = {};
	auto createResult = Kernel32::CreateProcessW(
		nullptr, cmdWide, nullptr, nullptr,
		hasRedirect,  // inherit handles only when redirecting
		0, nullptr, nullptr, &si, &pi);

	if (createResult.IsErr())
	{
		return Result<Process, Error>::Err(createResult, Error::Process_CreateFailed);
	}

	// Close the thread handle — we only need the process handle
	(VOID)NTDLL::ZwClose(pi.hThread);

	return Result<Process, Error>::Ok(
		Process((SSIZE)pi.dwProcessId, pi.hProcess));
}

// ============================================================================
// Process::Wait
// ============================================================================

Result<SSIZE, Error> Process::Wait() noexcept
{
	if (!IsValid())
		return Result<SSIZE, Error>::Err(Error::Process_WaitFailed);

	// Wait indefinitely for the process to exit
	auto waitResult = NTDLL::ZwWaitForSingleObject(handle, 0, nullptr);
	if (waitResult.IsErr())
	{
		return Result<SSIZE, Error>::Err(waitResult, Error::Process_WaitFailed);
	}

	// Get the exit code
	// ZwWaitForSingleObject returns STATUS_SUCCESS (0) when signaled
	// The actual exit code would require ZwQueryInformationProcess,
	// but for now return 0 on successful wait
	(VOID)NTDLL::ZwClose(handle);
	handle = nullptr;
	id = INVALID_ID;
	return Result<SSIZE, Error>::Ok(0);
}

// ============================================================================
// Process::Terminate
// ============================================================================

Result<VOID, Error> Process::Terminate() noexcept
{
	if (!IsValid())
		return Result<VOID, Error>::Err(Error::Process_TerminateFailed);

	auto result = NTDLL::ZwTerminateProcess(handle, 1);
	if (result.IsErr())
	{
		return Result<VOID, Error>::Err(result, Error::Process_TerminateFailed);
	}
	return Result<VOID, Error>::Ok();
}

// ============================================================================
// Process::IsRunning
// ============================================================================

BOOL Process::IsRunning() const noexcept
{
	if (!IsValid())
		return false;

	// Wait with zero timeout — STATUS_TIMEOUT means still running
	LARGE_INTEGER timeout = {};
	timeout.QuadPart = 0;
	auto result = NTDLL::ZwWaitForSingleObject(handle, 0, &timeout);
	if (result.IsErr())
		return false;

	return result.Value() == (NTSTATUS)STATUS_TIMEOUT;
}

// ============================================================================
// Process::Close
// ============================================================================

Result<VOID, Error> Process::Close() noexcept
{
	if (handle != nullptr)
	{
		(VOID)NTDLL::ZwClose(handle);
		handle = nullptr;
	}
	id = INVALID_ID;
	return Result<VOID, Error>::Ok();
}
