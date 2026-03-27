#include "platform/console/console.h"
#include "platform/platform.h"
#include "platform/kernel/windows/ntdll.h"
#include "platform/kernel/windows/peb.h"

/// @brief Write a span of wide characters to the console
/// @param text Span of wide characters to write to the console
/// @return Integer representing the number of characters written, or 0 on error
UINT32 Console::Write(Span<const CHAR> text)
{
	PPEB peb = GetCurrentPEB();
	IO_STATUS_BLOCK ioStatusBlock;
	Memory::Zero(&ioStatusBlock, sizeof(IO_STATUS_BLOCK));
	(VOID)NTDLL::ZwWriteFile(peb->ProcessParameters->StandardOutput, nullptr, nullptr, nullptr, &ioStatusBlock, (PVOID)text.Data(), text.Size(), nullptr, nullptr);
	return (UINT32)ioStatusBlock.Information;
}
