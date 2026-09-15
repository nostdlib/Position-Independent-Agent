#include "platform/console/console.h"
#include "platform/platform.h"
#include "platform/kernel/windows/ntdll.h"
#include "platform/kernel/windows/peb.h"
#include "platform/kernel/windows/kernel32.h"

/// @brief Write a span of narrow characters to the console
/// @param text Span of narrow characters to write to the console
/// @return Integer representing the number of characters written, or 0 on error
UINT32 Console::Write(Span<const CHAR> text)
{
	PPEB peb = GetCurrentPEB();
	PVOID handle = peb->ProcessParameters->StandardOutput;
	IO_STATUS_BLOCK ioStatusBlock;
	Memory::Zero(&ioStatusBlock, sizeof(IO_STATUS_BLOCK));

	// Pre-Windows 8 consoles are CSRSS pseudo-handles, not NT file handles (ConDrv
	// arrived in Windows 8), so ZwWriteFile rejects them; kernel32 WriteFile routes
	// them through the console subsystem instead.
	auto result = NTDLL::ZwWriteFile(handle, nullptr, nullptr, nullptr, &ioStatusBlock, (PVOID)text.Data(), (UINT32)text.Size(), nullptr, nullptr);
	if (!result)
	{
		auto fallback = Kernel32::WriteFile(handle, (PVOID)text.Data(), (UINT32)text.Size());
		return fallback ? fallback.Value() : 0;
	}
	return (UINT32)ioStatusBlock.Information;
}
