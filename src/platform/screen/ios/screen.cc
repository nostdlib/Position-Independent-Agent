/**
 * @file screen.cc
 * @brief iOS Screen Implementation (stub)
 *
 * @details iOS screen capture requires UIKit (Objective-C runtime) or
 * ReplayKit framework with special entitlements. Neither is accessible
 * from a position-independent runtime context without the Objective-C
 * message dispatch infrastructure. Both functions return appropriate
 * error codes.
 *
 * @note If iOS screen capture is needed in the future, it would require
 * adding Objective-C runtime resolution (objc_msgSend via dyld) and
 * UIKit/UIScreen class method invocation.
 */

#include "platform/screen/screen.h"

// =============================================================================
// Screen::GetDevices (iOS — not supported)
// =============================================================================

Result<ScreenDeviceList, Error> Screen::GetDevices()
{
	return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_GetDevicesFailed));
}

// =============================================================================
// Screen::Capture (iOS — not supported)
// =============================================================================

Result<VOID, Error> Screen::Capture([[maybe_unused]] const ScreenDevice &device, [[maybe_unused]] Span<RGB> buffer)
{
	return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
}
