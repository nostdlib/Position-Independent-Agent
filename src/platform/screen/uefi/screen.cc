/**
 * @file screen.cc
 * @brief UEFI Screen Implementation
 *
 * @details Implements screen device enumeration and capture via the EFI
 * Graphics Output Protocol (GOP). GetDevices() locates the GOP protocol
 * and reports the current video mode as the single display device.
 * Capture() uses the GOP Blt() function with EfiBltVideoToBltBuffer to
 * copy the framebuffer contents into an RGB pixel buffer.
 *
 * @see UEFI Specification 2.10 — Section 12.9, Graphics Output Protocol
 *      https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html#efi-graphics-output-protocol
 */

#include "platform/screen/screen.h"
#include "platform/kernel/uefi/efi_context.h"
#include "platform/kernel/uefi/efi_graphics_output_protocol.h"

// =============================================================================
// Internal helper
// =============================================================================

/// @brief Construct the GOP protocol GUID without leaking to .rdata
/// @return EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID {9042A9DE-23DC-4A38-96FB-7ADED080516A}
static NOINLINE EFI_GUID MakeGopGuid()
{
	EFI_GUID g;
	g.Data1 = 0x9042A9DE;
	g.Data2 = 0x23DC;
	g.Data3 = 0x4A38;
	g.Data4[0] = 0x96; g.Data4[1] = 0xFB; g.Data4[2] = 0x7A; g.Data4[3] = 0xDE;
	g.Data4[4] = 0xD0; g.Data4[5] = 0x80; g.Data4[6] = 0x51; g.Data4[7] = 0x6A;
	return g;
}

/// @brief Locate the GOP protocol via BootServices->LocateProtocol
/// @return Pointer to the GOP interface, or nullptr on failure
static EFI_GRAPHICS_OUTPUT_PROTOCOL *LocateGop()
{
	EFI_CONTEXT *ctx = GetEfiContext();
	if (ctx == nullptr || ctx->SystemTable == nullptr)
		return nullptr;

	EFI_BOOT_SERVICES *bs = ctx->SystemTable->BootServices;
	if (bs == nullptr)
		return nullptr;

	EFI_GUID gopGuid = MakeGopGuid();
	PVOID interface = nullptr;

	EFI_STATUS status = bs->LocateProtocol(&gopGuid, nullptr, &interface);
	if (EFI_ERROR_CHECK(status))
		return nullptr;

	return (EFI_GRAPHICS_OUTPUT_PROTOCOL *)interface;
}

// =============================================================================
// Screen::GetDevices
// =============================================================================

Result<ScreenDeviceList, Error> Screen::GetDevices()
{
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = LocateGop();
	if (gop == nullptr || gop->Mode == nullptr || gop->Mode->Info == nullptr)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_GetDevicesFailed));

	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;

	if (info->HorizontalResolution == 0 || info->VerticalResolution == 0)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_GetDevicesFailed));

	ScreenDevice *devices = new ScreenDevice[1];
	if (devices == nullptr)
		return Result<ScreenDeviceList, Error>::Err(Error(Error::Screen_AllocFailed));

	devices[0].Left = 0;
	devices[0].Top = 0;
	devices[0].Width = info->HorizontalResolution;
	devices[0].Height = info->VerticalResolution;
	devices[0].Primary = true;

	ScreenDeviceList list;
	list.Devices = devices;
	list.Count = 1;
	return Result<ScreenDeviceList, Error>::Ok(list);
}

// =============================================================================
// Screen::Capture
// =============================================================================

Result<VOID, Error> Screen::Capture(const ScreenDevice &device, Span<RGB> buffer)
{
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = LocateGop();
	if (gop == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));

	UINT32 width = device.Width;
	UINT32 height = device.Height;
	UINT32 pixelCount = width * height;

	// Allocate temporary BLT buffer (BGRX format, 4 bytes per pixel)
	EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bltBuf = new EFI_GRAPHICS_OUTPUT_BLT_PIXEL[pixelCount];
	if (bltBuf == nullptr)
		return Result<VOID, Error>::Err(Error(Error::Screen_AllocFailed));

	// Copy from video framebuffer to BLT buffer
	EFI_STATUS status = gop->Blt(
		gop,
		bltBuf,
		EfiBltVideoToBltBuffer,
		(USIZE)device.Left,  // SourceX
		(USIZE)device.Top,   // SourceY
		0,                   // DestinationX
		0,                   // DestinationY
		(USIZE)width,
		(USIZE)height,
		0);                  // Delta (0 = Width * sizeof(BLT_PIXEL))

	if (EFI_ERROR_CHECK(status))
	{
		delete[] bltBuf;
		return Result<VOID, Error>::Err(Error(Error::Screen_CaptureFailed));
	}

	// Convert BLT pixel (BGRX) to RGB
	PRGB rgbBuf = buffer.Data();
	for (UINT32 i = 0; i < pixelCount; i++)
	{
		rgbBuf[i].Red = bltBuf[i].Red;
		rgbBuf[i].Green = bltBuf[i].Green;
		rgbBuf[i].Blue = bltBuf[i].Blue;
	}

	delete[] bltBuf;
	return Result<VOID, Error>::Ok();
}
