/**
 * date_time.cc - UEFI Date/Time Implementation
 *
 * Provides DateTime::Now and GetMonotonicNanoseconds using
 * EFI Runtime Services and hardware timestamps.
 */

#include "platform/system/date_time.h"
#include "platform/kernel/uefi/efi_context.h"

/**
 * DateTime::Now - Get current date and time from UEFI
 *
 * Uses EFI_RUNTIME_SERVICES->GetTime to retrieve the current time.
 *
 * @return DateTime structure with current date/time
 */
DateTime DateTime::Now()
{
	DateTime dt{};

	EFI_CONTEXT *ctx = GetEfiContext();
	EFI_RUNTIME_SERVICES *rs = ctx->SystemTable->RuntimeServices;

	// Initalizing Time structure to be filled by GetTime
	EFI_TIME efiTime{};
	EFI_STATUS status = rs->GetTime(&efiTime, nullptr);
	
	if (status == EFI_SUCCESS)
	{
		dt.Years = efiTime.Year;
		dt.Months = efiTime.Month;
		dt.Days = efiTime.Day;
		dt.Hours = efiTime.Hour;
		dt.Minutes = efiTime.Minute;
		dt.Seconds = efiTime.Second;
		dt.Milliseconds = efiTime.Nanosecond / 1000000;
		dt.Microseconds = (efiTime.Nanosecond / 1000) % 1000;
		dt.Nanoseconds = efiTime.Nanosecond % 1000;
	}
	else
	{
		// Default to epoch-like value on failure
		dt.Years = 1970;
		dt.Months = 1;
		dt.Days = 1;
		dt.Hours = 0;
		dt.Minutes = 0;
		dt.Seconds = 0;
		dt.Nanoseconds = 0;
		dt.Milliseconds = 0;
		dt.Microseconds = 0;
	}

	return dt;
}

/**
 * DateTime::GetMonotonicNanoseconds - Get monotonic timestamp
 *
 * Uses CPU timestamp counter for high-resolution monotonic time.
 * This is used for entropy collection and timing measurements.
 *
 * @return Monotonic nanosecond count (relative, not wall-clock)
 */
UINT64 DateTime::GetMonotonicNanoseconds()
{
#if defined(ARCHITECTURE_X86_64) || defined(ARCHITECTURE_I386)
	// Use RDTSC instruction for x86 architectures
	UINT32 lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((UINT64)hi << 32) | lo;
#elif defined(ARCHITECTURE_AARCH64)
	// Use CNTVCT_EL0 register for AArch64
	UINT64 val;
	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
	return val;
#else
	// Fallback: use GetTime and monotonic count
	EFI_CONTEXT *ctx = GetEfiContext();
	EFI_BOOT_SERVICES *bs = ctx->SystemTable->BootServices;

	UINT64 count = 0;
	bs->GetNextMonotonicCount(&count);
	return count * 100; // Approximate conversion
#endif
}
