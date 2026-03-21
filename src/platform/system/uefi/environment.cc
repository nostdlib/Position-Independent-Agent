/**
 * environment.cc - UEFI Environment Variable and Platform Stub
 *
 * UEFI does not have traditional environment variables or a versioned OS.
 * This stub always returns empty/not found for variables, and "uefi" for
 * platform identification.
 */

#include "platform/system/environment.h"
#include "core/string/string.h"

USIZE Environment::GetVariable([[maybe_unused]] const CHAR* name, Span<CHAR> buffer) noexcept
{
	if (buffer.Size() > 0)
	{
		buffer[0] = '\0';
	}

	return 0;
}

USIZE Environment::GetAgentPlatform(Span<CHAR> buffer) noexcept
{
	StringUtils::Copy(buffer, Span<const CHAR>("uefi"));
	return StringUtils::Length(buffer.Data());
}

USIZE Environment::GetOSVersion(Span<CHAR> buffer) noexcept
{
	StringUtils::Copy(buffer, Span<const CHAR>("uefi"));
	return StringUtils::Length(buffer.Data());
}
