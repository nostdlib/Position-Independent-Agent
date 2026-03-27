/**
 * @file platform_result.h
 * @brief FreeBSD syscall result to Result<T, Error> conversion.
 *
 * @details Provides the FromFreeBSD() helper that converts raw FreeBSD BSD
 * syscall return values into the PIR Result<T, Error> type. After the
 * carry-flag negation in the System::Call wrappers, negative return values
 * indicate errors; this function maps them to Result::Err with a POSIX
 * error code.
 */
#pragma once

#include "core/types/result.h"
#include "core/types/error.h"

namespace result
{

/// FreeBSD syscall: success when result >= 0, failure stores -result as errno.
template <typename T>
[[nodiscard]] FORCE_INLINE Result<T, Error> FromFreeBSD(SSIZE result) noexcept
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
