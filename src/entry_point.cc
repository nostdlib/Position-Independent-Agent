/**
 * entry_point.cc - CPP-PIC Runtime Entry Point
 *
 * Unified entry_point() entry point for all platforms.
 */

#include "lib/runtime.h"

INT32 start();

/**
 * entry_point - Entry point for all platforms
 */
#if defined(PLATFORM_UEFI)
ENTRYPOINT EFI_STATUS EFIAPI entry_point(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
#else
ENTRYPOINT INT32 entry_point(VOID)
#endif
{
#if defined(PLATFORM_UEFI)
	// Allocate context on stack and store pointer in CPU register (GS/TPIDR_EL0)
	// This eliminates the need for a global variable in .data section
	EFI_CONTEXT efiContext = {};
	efiContext.ImageHandle = ImageHandle;
	efiContext.SystemTable = SystemTable;
	SetEfiContextRegister(efiContext);
	// Disable watchdog timer (default is 5 minutes)
	SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, nullptr);
#endif
	// Run runtime and unit tests
	INT32 exitCode = start();
	ExitProcess(exitCode);
}
