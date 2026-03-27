/**
 * @file platform_result.h
 * @brief Solaris syscall result to Result<T, Error> conversion.
 *
 * @details Provides the FromSolaris() helper that converts raw Solaris syscall
 * return values into the PIR Result<T, Error> type. The carry-flag negation
 * happens in the System::Call wrappers, so by the time values reach this
 * function, negative return values indicate errors -- matching the Linux/macOS
 * convention used throughout PIR.
 */
#pragma once

#include "core/types/result.h"
#include "core/types/error.h"

namespace result
{

/// Solaris syscall: success when result >= 0, failure stores -result as errno.
/// The carry-flag negation happens in system.h, so by the time we reach here,
/// negative return = error (same convention as FromLinux/FromMacOS).
template <typename T>
[[nodiscard]] FORCE_INLINE Result<T, Error> FromSolaris(SSIZE result) noexcept
{
	if (result >= 0)
	{
		if constexpr (__is_same_as(T, VOID))
			return Result<T, Error>::Ok();
		else
			return Result<T, Error>::Ok((T)result);
	}
	return Result<T, Error>::Err(Error::Posix((UINT32)(-result)));
}

} // namespace result
