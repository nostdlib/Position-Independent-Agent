#include "platform/kernel/windows/gdi32.h"
#include "platform/platform.h"
#include "platform/kernel/windows/peb.h"

#define ResolveGdi32ExportAddress(functionName) ResolveExportAddress((const WCHAR *)L"gdi32.dll", Djb2::HashCompileTime(functionName))

PVOID Gdi32::CreateCompatibleDC(PVOID hdc)
{
	auto fn = (PVOID(STDCALL *)(PVOID))ResolveGdi32ExportAddress("CreateCompatibleDC");
	if (fn == nullptr)
		return nullptr;
	return fn(hdc);
}

PVOID Gdi32::CreateDCW(PCWCHAR lpszDriver, PCWCHAR lpszDevice, PCWCHAR lpszOutput, PCVOID lpInitData)
{
	auto fn = (PVOID(STDCALL *)(PCWCHAR, PCWCHAR, PCWCHAR, PCVOID))ResolveGdi32ExportAddress("CreateDCW");
	if (fn == nullptr)
		return nullptr;
	return fn(lpszDriver, lpszDevice, lpszOutput, lpInitData);
}

PVOID Gdi32::CreateCompatibleBitmap(PVOID hdc, INT32 cx, INT32 cy)
{
	auto fn = (PVOID(STDCALL *)(PVOID, INT32, INT32))ResolveGdi32ExportAddress("CreateCompatibleBitmap");
	if (fn == nullptr)
		return nullptr;
	return fn(hdc, cx, cy);
}

PVOID Gdi32::CreateDIBSection(PVOID hdc, const BITMAPINFO *pbmi, UINT32 usage, PPVOID ppvBits, PVOID hSection, UINT32 offset)
{
	auto fn = (PVOID(STDCALL *)(PVOID, const BITMAPINFO *, UINT32, PPVOID, PVOID, UINT32))ResolveGdi32ExportAddress("CreateDIBSection");
	if (fn == nullptr)
		return nullptr;
	return fn(hdc, pbmi, usage, ppvBits, hSection, offset);
}

PVOID Gdi32::SelectObject(PVOID hdc, PVOID h)
{
	auto fn = (PVOID(STDCALL *)(PVOID, PVOID))ResolveGdi32ExportAddress("SelectObject");
	if (fn == nullptr)
		return nullptr;
	return fn(hdc, h);
}

BOOL Gdi32::BitBlt(PVOID hdc, INT32 x, INT32 y, INT32 cx, INT32 cy, PVOID hdcSrc, INT32 x1, INT32 y1, UINT32 rop)
{
	auto fn = (BOOL(STDCALL *)(PVOID, INT32, INT32, INT32, INT32, PVOID, INT32, INT32, UINT32))ResolveGdi32ExportAddress("BitBlt");
	if (fn == nullptr)
		return false;
	return fn(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
}

INT32 Gdi32::SetStretchBltMode(PVOID hdc, INT32 mode)
{
	auto fn = (INT32(STDCALL *)(PVOID, INT32))ResolveGdi32ExportAddress("SetStretchBltMode");
	if (fn == nullptr)
		return 0;
	return fn(hdc, mode);
}

BOOL Gdi32::StretchBlt(PVOID hdc, INT32 x, INT32 y, INT32 w, INT32 h, PVOID hdcSrc, INT32 x1, INT32 y1, INT32 w1, INT32 h1, UINT32 rop)
{
	auto fn = (BOOL(STDCALL *)(PVOID, INT32, INT32, INT32, INT32, PVOID, INT32, INT32, INT32, INT32, UINT32))ResolveGdi32ExportAddress("StretchBlt");
	if (fn == nullptr)
		return false;
	return fn(hdc, x, y, w, h, hdcSrc, x1, y1, w1, h1, rop);
}

INT32 Gdi32::GetDIBits(PVOID hdc, PVOID hbm, UINT32 start, UINT32 cLines, PVOID lpvBits, PBITMAPINFOHEADER lpbmi, UINT32 usage)
{
	auto fn = (INT32(STDCALL *)(PVOID, PVOID, UINT32, UINT32, PVOID, PBITMAPINFOHEADER, UINT32))ResolveGdi32ExportAddress("GetDIBits");
	if (fn == nullptr)
		return 0;
	return fn(hdc, hbm, start, cLines, lpvBits, lpbmi, usage);
}

BOOL Gdi32::DeleteDC(PVOID hdc)
{
	auto fn = (BOOL(STDCALL *)(PVOID))ResolveGdi32ExportAddress("DeleteDC");
	if (fn == nullptr)
		return false;
	return fn(hdc);
}

BOOL Gdi32::DeleteObject(PVOID ho)
{
	auto fn = (BOOL(STDCALL *)(PVOID))ResolveGdi32ExportAddress("DeleteObject");
	if (fn == nullptr)
		return false;
	return fn(ho);
}
