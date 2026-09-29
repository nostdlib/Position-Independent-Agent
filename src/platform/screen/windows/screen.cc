/**
 * @file screen.cc
 * @brief Windows Screen Implementation
 *
 * @details Implements screen device enumeration via User32
 * EnumDisplayDevicesW/EnumDisplaySettingsW and screen capture via
 * GDI CreateCompatibleDC/BitBlt into a DIB section (with automatic fallback
 * to the CreateCompatibleBitmap + GetDIBits path when a driver rejects the
 * DIB destination). User32 and Gdi32 wrappers auto-load their DLLs via
 * ResolveExportAddress when not already loaded. Each stateful capture logs a
 * per-stage timing line so live runs show where the frame time goes.
 *
 * @see EnumDisplayDevicesW
 *      https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaydevicesw
 * @see EnumDisplaySettingsW
 *      https://learn.microsoft.com/en-us/windows/winuser/nf-winuser-enumdisplaysettingsw
 * @see BitBlt
 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt
 * @see CreateDIBSection
 *      https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createdibsection
 */

#include "platform/screen/screen.h"
#include "platform/kernel/windows/user32.h"
#include "platform/kernel/windows/gdi32.h"
#include "platform/console/logger.h"
#include "platform/system/date_time.h"

// =============================================================================
// Screen::GetDevices
// =============================================================================

Result<ScreenDeviceList, Error> Screen::GetDevices()
{
	// Enumerate active displays (stack buffer for up to 16)
	constexpr UINT32 maxDevices = 16;
	ScreenDevice tempDevices[maxDevices];
	UINT32 deviceCount = 0;

	for (UINT32 i = 0; i < maxDevices; i++)
	{
		DISPLAY_DEVICEW dd;
		Memory::Zero(&dd, sizeof(dd));
		dd.cb = sizeof(DISPLAY_DEVICEW);

		if (!User32::EnumDisplayDevicesW(nullptr, i, &dd, 0))
			break;

		if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE))
			continue;

		DEVMODEW dm;
		Memory::Zero(&dm, sizeof(dm));
		dm.dmSize = sizeof(DEVMODEW);

		if (!User32::EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
			continue;

		tempDevices[deviceCount].Left = dm.dmPositionX;
		tempDevices[deviceCount].Top = dm.dmPositionY;
		tempDevices[deviceCount].Width = dm.dmPelsWidth;
		tempDevices[deviceCount].Height = dm.dmPelsHeight;
		tempDevices[deviceCount].Primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
		deviceCount++;
	}

	if (deviceCount == 0)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_GetDevicesFailed));

	ScreenDevice *devices = new ScreenDevice[deviceCount];
	if (devices == nullptr)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_AllocFailed));

	Memory::Copy(devices, tempDevices, deviceCount * sizeof(ScreenDevice));

	ScreenDeviceList list;
	list.Devices = devices;
	list.Count = deviceCount;
	return Result<ScreenDeviceList, Error>::Ok(list);
}

// =============================================================================
// Screen::Capture
// =============================================================================

// Persistent per-display capture resources: repeated captures reuse one memory
// DC, a cached screen DC, and a capture bitmap. In DIB mode the bitmap is a
// DIB section whose pixel memory IS the BGRA buffer (BitBlt lands pixels
// directly in it — no GetDIBits round trip); in DDB mode it is a compatible
// bitmap read back with GetDIBits into a separate heap buffer. Everything is
// rebuilt when the display mode changes or a blt goes stale
struct WinCaptureState
{
	PVOID memDC;
	PVOID screenDC;          ///< Cached GetDC(nullptr) handle; re-borrowed on staleness
	PVOID bitmap;            ///< DIB section (DIB mode) or compatible bitmap (DDB mode)
	PVOID oldBitmap;
	BITMAPINFOHEADER bmi;
	UINT8 *bgra;             ///< DIB-section bits or heap readback buffer (see bgraIsDibSection)
	UINT32 bgraSize;
	INT32 width;
	INT32 height;
	BOOL useDib;             ///< DIB-section blt target; flipped off on driver rejection
	BOOL bgraIsDibSection;   ///< bgra freed via DeleteObject (true) or delete[] (false)
};

// Deselect, delete, and null the owned GDI objects and pixel buffer. A DIB
// section's memory dies with its handle; the DDB-mode readback buffer is heap
static VOID ReleaseGdiObjects(WinCaptureState *state)
{
	if (state->memDC != nullptr && state->oldBitmap != nullptr)
	{
		Gdi32::SelectObject(state->memDC, state->oldBitmap);
		state->oldBitmap = nullptr;
	}
	if (state->bitmap != nullptr)
	{
		Gdi32::DeleteObject(state->bitmap);
		state->bitmap = nullptr;
	}
	if (state->bgra != nullptr && !state->bgraIsDibSection)
		delete[] state->bgra;
	state->bgra = nullptr;
	state->bgraSize = 0;
	state->bgraIsDibSection = false;
	if (state->memDC != nullptr)
	{
		Gdi32::DeleteDC(state->memDC);
		state->memDC = nullptr;
	}
}

// Drop the cached screen DC; the next acquire borrows a fresh one
static VOID DropScreenDC(WinCaptureState *state)
{
	if (state->screenDC != nullptr)
	{
		User32::ReleaseDC(nullptr, state->screenDC);
		state->screenDC = nullptr;
	}
}

// Cached virtual-screen DC (GetDC(nullptr)) — caching removes the per-frame
// GetDC/ReleaseDC kernel round trips; a failed blt drops it for a fresh one
static PVOID AcquireScreenDC(WinCaptureState *state)
{
	if (state->screenDC == nullptr)
		state->screenDC = User32::GetDC(nullptr);
	return state->screenDC;
}

// (Re)create the memory DC + capture bitmap for the current mode. DIB mode:
// the section's bits pointer becomes state->bgra, so blts into the DC write
// the capture straight into our buffer. DDB mode: plain compatible bitmap,
// read back with GetDIBits in Capture
static BOOL RebuildGdiObjects(WinCaptureState *state, INT32 width, INT32 height)
{
	ReleaseGdiObjects(state);

	PVOID screenDC = AcquireScreenDC(state);
	if (screenDC == nullptr)
		return false;

	state->memDC = Gdi32::CreateCompatibleDC(screenDC);
	if (state->memDC != nullptr)
	{
		// 32bpp top-down format shared by both modes (DIB target / GetDIBits)
		Memory::Zero(&state->bmi, sizeof(state->bmi));
		state->bmi.biSize = sizeof(BITMAPINFOHEADER);
		state->bmi.biWidth = width;
		state->bmi.biHeight = -height; // negative = top-down scanlines
		state->bmi.biPlanes = 1;
		state->bmi.biBitCount = 32;
		state->bmi.biCompression = BI_RGB;

		if (state->useDib)
		{
			// No color table at 32bpp BI_RGB, so the header alone is the BITMAPINFO
			PVOID bits = nullptr;
			BITMAPINFO *info = (BITMAPINFO *)&state->bmi;
			state->bitmap = Gdi32::CreateDIBSection(screenDC, info, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (state->bitmap != nullptr && bits != nullptr)
			{
				state->bgra = (UINT8 *)bits;
				state->bgraSize = (UINT32)width * (UINT32)height * 4;
				state->bgraIsDibSection = true;
				state->oldBitmap = Gdi32::SelectObject(state->memDC, state->bitmap);
			}
		}
		else
		{
			state->bitmap = Gdi32::CreateCompatibleBitmap(screenDC, width, height);
			if (state->bitmap != nullptr)
				state->oldBitmap = Gdi32::SelectObject(state->memDC, state->bitmap);
		}
	}

	BOOL ok = state->memDC != nullptr && state->bitmap != nullptr && state->oldBitmap != nullptr &&
	          (!state->useDib || state->bgra != nullptr);
	if (!ok)
	{
		ReleaseGdiObjects(state);
		return false;
	}

	state->width = width;
	state->height = height;
	return true;
}

// Rebuild with automatic degradation: some drivers reject the DIB-section
// blt target (observed for non-primary monitors) — capture must still work,
// so fall back to the proven DDB + GetDIBits path before reporting failure
static BOOL RebuildCaptureObjects(WinCaptureState *state, INT32 width, INT32 height)
{
	if (RebuildGdiObjects(state, width, height))
		return true;
	if (state->useDib)
	{
		state->useDib = false;
		LOG_WARNING("dib-section capture rejected, falling back to getdibits");
		return RebuildGdiObjects(state, width, height);
	}
	return false;
}

Result<PVOID, Error> Screen::CreateCaptureState(const ScreenDevice &device)
{
	WinCaptureState *state = new WinCaptureState();
	if (state == nullptr)
		return Result<PVOID, Error>::Err(Error(Error::Screen_AllocFailed));
	Memory::Zero(state, sizeof(WinCaptureState));
	state->useDib = true;

	if (!RebuildCaptureObjects(state, (INT32)device.Width, (INT32)device.Height))
	{
		DropScreenDC(state);
		delete state;
		return Result<PVOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	return Result<PVOID, Error>::Ok((PVOID)state);
}

VOID Screen::DestroyCaptureState(PVOID captureState)
{
	if (captureState == nullptr)
		return;

	WinCaptureState *state = (WinCaptureState *)captureState;
	ReleaseGdiObjects(state);
	DropScreenDC(state);
	delete state;
}

/// @brief Convert a top-down 32bpp BGRA buffer to packed RGB in one pass
/// @details Four pixels per iteration pack into three dword stores instead
///          of twelve byte stores. An SSE2 shuffle network was measured
///          slower here (0.81 vs 0.67 ms per 1080p frame): the stage is
///          memory-bandwidth-bound, so fewer stores beat wider loads.
/// @param bgra Capture pixel memory — DIB-section bits or GetDIBits buffer
/// @param rgb Destination frame buffer (3 bytes per pixel)
/// @param pixelCount Total pixels to convert
static VOID ConvertBgraToRgb(const UINT8 *bgra, PRGB rgb, UINT32 pixelCount)
{
	UINT32 i = 0;
	for (; i + 4 <= pixelCount; i += 4, bgra += 16)
	{
		const UINT32 *src = (const UINT32 *)bgra;
		UINT32 *out = (UINT32 *)(rgb + i);
		UINT32 d0 = src[0], d1 = src[1], d2 = src[2], d3 = src[3];
		UINT32 r0 = (d0 >> 16) & 0xFF, g0 = (d0 >> 8) & 0xFF;
		UINT32 r1 = (d1 >> 16) & 0xFF, g1 = (d1 >> 8) & 0xFF, b1 = d1 & 0xFF;
		UINT32 r2 = (d2 >> 16) & 0xFF, g2 = (d2 >> 8) & 0xFF, b2 = d2 & 0xFF;
		UINT32 r3 = (d3 >> 16) & 0xFF, g3 = (d3 >> 8) & 0xFF, b3 = d3 & 0xFF;
		out[0] = r0 | (g0 << 8) | ((d0 & 0xFF) << 16) | (r1 << 24);
		out[1] = g1 | (b1 << 8) | (r2 << 16) | (g2 << 24);
		out[2] = b2 | (r3 << 8) | (g3 << 16) | (b3 << 24);
	}

	// Tail (last <4 pixels)
	for (; i < pixelCount; i++, bgra += 4)
	{
		rgb[i].Red = bgra[2];
		rgb[i].Green = bgra[1];
		rgb[i].Blue = bgra[0];
	}
}

Result<VOID, Error> Screen::Capture(const ScreenDevice &device, Span<RGB> buffer, PVOID captureState)
{
	// One-shot callers (tests, single captures) run the same sequence through
	// a temporary state — one GDI pipeline to maintain
	if (captureState == nullptr)
	{
		auto state = CreateCaptureState(device);
		if (!state)
			return Result<VOID, Error>::Err(state.Error());
		auto result = Capture(device, buffer, state.Value());
		DestroyCaptureState(state.Value());
		return result;
	}

	WinCaptureState *state = (WinCaptureState *)captureState;
	INT32 width = (INT32)device.Width;
	INT32 height = (INT32)device.Height;

	// Display-mode change since the state was built: rebuild everything
	if (width != state->width || height != state->height)
	{
		if (!RebuildCaptureObjects(state, width, height))
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Persistent objects may go stale (lost DC, driver hiccup, desktop switch
	// invalidating the cached screen DC): drop the cached DC, rebuild once,
	// and retry before reporting failure. blt measures the BitBlt itself;
	// dibits stays 0 in DIB mode (captures land in bgra directly)
	[[maybe_unused]] UINT64 bltNs = 0;
	[[maybe_unused]] UINT64 dibitsNs = 0;
	for (UINT32 attempt = 0; ; attempt++)
	{
		PVOID screenDC = AcquireScreenDC(state);
		if (screenDC == nullptr)
		{
			DropScreenDC(state);
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
		}

		UINT64 stage = DateTime::GetMonotonicNanoseconds();
		BOOL blit = Gdi32::BitBlt(state->memDC, 0, 0, width, height,
			screenDC, device.Left, device.Top, SRCCOPY);
		bltNs = DateTime::GetMonotonicNanoseconds() - stage;

		if (!blit)
		{
			if (attempt >= 1)
				return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

			// Cached DC may be stale; a DIB target being rejected outright
			// degrades to the DDB + GetDIBits path for good
			DropScreenDC(state);
			if (state->useDib)
			{
				state->useDib = false;
				LOG_WARNING("dib-section blt rejected, falling back to getdibits");
			}
			if (!RebuildGdiObjects(state, width, height))
				return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
			continue;
		}

		if (state->useDib)
			break; // pixels are in state->bgra already

		// DDB destination: read the pixels back. GetDIBits requires the bitmap
		// not be selected into a DC (documented precondition — some drivers
		// enforce it)
		UINT32 bgraNeeded = (UINT32)width * (UINT32)height * 4;
		if (state->bgraSize < bgraNeeded)
		{
			UINT8 *grown = new UINT8[bgraNeeded];
			if (grown == nullptr)
				return Result<VOID, Error>::Err(Error(Error::Screen_AllocFailed));
			delete[] state->bgra; // heap-only here (DIB-mode memory died with the bitmap)
			state->bgra = grown;
			state->bgraSize = bgraNeeded;
		}

		stage = DateTime::GetMonotonicNanoseconds();
		Gdi32::SelectObject(state->memDC, state->oldBitmap);
		INT32 scanLines = Gdi32::GetDIBits(state->memDC, state->bitmap, 0, (UINT32)height,
			state->bgra, &state->bmi, DIB_RGB_COLORS);
		Gdi32::SelectObject(state->memDC, state->bitmap);
		dibitsNs = DateTime::GetMonotonicNanoseconds() - stage;
		if (scanLines != 0)
			break;

		if (attempt >= 1)
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

		DropScreenDC(state);
		if (!RebuildGdiObjects(state, width, height))
			return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// BGRA -> RGB straight out of the capture memory into the frame buffer
	UINT32 pixelCount = device.Width * device.Height;
	UINT64 stage = DateTime::GetMonotonicNanoseconds();
	ConvertBgraToRgb(state->bgra, buffer.Data(), pixelCount);
	[[maybe_unused]] UINT64 convertNs = DateTime::GetMonotonicNanoseconds() - stage;

	LOG_INFO("[capture] blt %u ms, dibits %u ms, convert %u ms",
	         (UINT32)(bltNs / 1000000), (UINT32)(dibitsNs / 1000000),
	         (UINT32)(convertNs / 1000000));

	return Result<VOID, Error>::Ok();
}
