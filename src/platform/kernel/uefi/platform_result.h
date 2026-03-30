/**
 * @file platform_result.h
 * @brief UEFI EFI_STATUS to Result<T, Error> conversion utilities.
 *
 * @details Provides the FromEfiStatus() helper in the result namespace, which converts
 *          UEFI EFI_STATUS codes into the PIR Result<T, Error> type. Success is determined
 *          by the sign bit of the status value: non-negative values indicate success, while
 *          negative values (high bit set) indicate an error per the UEFI status code convention.
 *
 * @see UEFI Specification 2.10 — Appendix D, Status Codes
 */
#pragma once

#include "core/types/result.h"
#include "core/types/error.h"

namespace result
{

/// UEFI EFI_STATUS: success when (SSIZE)status >= 0.
template <typename T>
[[nodiscard]] FORCE_INLINE Result<T, Error> FromEfiStatus(USIZE status) noexcept
{
	if ((SSIZE)status >= 0)
	{
		if constexpr (__is_same_as(T, VOID))
			return Result<T, Error>::Ok();
		else
			return Result<T, Error>::Ok((T)status);
	}
	// Strip the EFI error mask (bit 63) — the error code index fits in 32 bits,
	// and the Err path + PlatformKind::Uefi already convey error status.
	return Result<T, Error>::Err(Error::Uefi((UINT32)(status & 0x7FFFFFFFFFFFFFFF)));
}

} // namespace result
