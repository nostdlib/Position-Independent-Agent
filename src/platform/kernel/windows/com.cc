#include "platform/kernel/windows/com.h"
#include "platform/platform.h"
#include "platform/kernel/windows/peb.h"
#include "core/algorithms/djb2.h"

#define COINIT_MULTITHREADED 0x0 ///< Apartment model: concurrency + no message pump dependency
#define CLSCTX_INPROC_SERVER 0x1 ///< In-process server — the only context WPD supports here
#define S_OK 0x00000000u
#define S_FALSE 0x00000001u

// Resolves a COM export per call: combase.dll (Windows 7+) first, then the
// ole32.dll fallback. No caching — the address is re-walked on every use.
static PVOID ResolveComExportByHash(UINT64 functionNameHash)
{
	PVOID address = ResolveExportAddress((const WCHAR *)L"combase.dll", functionNameHash);
	if (address != nullptr)
		return address;
	return ResolveExportAddress((const WCHAR *)L"ole32.dll", functionNameHash);
}

// The hash must be computed from a string literal (consteval), hence the macro.
#define ResolveComExport(functionName) ResolveComExportByHash(Djb2::HashCompileTime(functionName))

Result<VOID, Error> Com::Initialize()
{
	auto fn = (UINT32(STDCALL *)(PVOID, UINT32))ResolveComExport("CoInitializeEx");
	if (fn == nullptr)
		return Result<VOID, Error>::Err(Error::Windows(0x80004005u)); // E_FAIL: no COM runtime export
	UINT32 hr = fn(nullptr, COINIT_MULTITHREADED);
	if (hr != S_OK && hr != S_FALSE)
		return Result<VOID, Error>::Err(Error::Windows(hr));
	return Result<VOID, Error>::Ok();
}

VOID Com::Uninitialize()
{
	auto fn = (VOID(STDCALL *)(VOID))ResolveComExport("CoUninitialize");
	if (fn != nullptr)
		fn();
}

Result<PVOID, Error> Com::CreateInstance(const GUID &clsid, const GUID &iid)
{
	auto fn = (UINT32(STDCALL *)(const GUID *, PVOID, UINT32, const GUID *, PVOID *))ResolveComExport("CoCreateInstance");
	if (fn == nullptr)
		return Result<PVOID, Error>::Err(Error::Windows(0x80004005u));
	PVOID out = nullptr;
	UINT32 hr = fn(&clsid, nullptr, CLSCTX_INPROC_SERVER, &iid, &out);
	if (hr != S_OK || out == nullptr)
		return Result<PVOID, Error>::Err(Error::Windows(hr));
	return Result<PVOID, Error>::Ok(out);
}

VOID Com::FreeMemory(PVOID memory)
{
	auto fn = (VOID(STDCALL *)(PVOID))ResolveComExport("CoTaskMemFree");
	if (fn != nullptr)
		fn(memory);
}

VOID Com::PropVariantClear(PVOID variant)
{
	auto fn = (UINT32(STDCALL *)(PVOID))ResolveComExport("PropVariantClear");
	if (fn != nullptr)
		(VOID)fn(variant);
}
