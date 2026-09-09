/**
 * @file com.h
 * @brief COM Runtime Initialization Wrappers
 *
 * @details Provides position-independent wrappers around the COM runtime
 * exports (CoInitializeEx, CoUninitialize, CoCreateInstance, CoTaskMemFree)
 * used by the portable-device (WPD) file-system layer. All function addresses
 * are resolved per call via ResolveExportAddress() — combase.dll first,
 * falling back to ole32.dll — using DJB2 hash-based PEB module lookup, so no
 * static import table entries are created.
 *
 * @note COM interfaces are hand-declared C-style structs in the WPD layer;
 * this header only supplies the runtime plumbing and the GUID type.
 *
 * @see CoInitializeEx
 *      https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex
 * @see CoCreateInstance
 *      https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cocreateinstance
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/error.h"
#include "core/types/result.h"

/**
 * @brief Globally unique identifier (SDK GUID layout).
 *
 * @details Binary layout matches the Windows SDK GUID structure so it can be
 * passed by address to COM APIs (REFIID/REFCLSID are pointers in the C ABI).
 *
 * @see GUID structure
 *      https://learn.microsoft.com/en-us/windows/win32/api/guiddef/ns-guiddef-guid
 */
typedef struct _GUID
{
	UINT32 Data1;   ///< First 4 bytes (little-endian in the registry string form)
	UINT16 Data2;   ///< Next 2 bytes
	UINT16 Data3;   ///< Next 2 bytes
	UINT8 Data4[8]; ///< Remaining 8 bytes
} GUID, *PGUID;

/**
 * @brief Wrappers for COM runtime exports (combase.dll / ole32.dll).
 *
 * @details Every method resolves its export per call — nothing is cached —
 * keeping the runtime free of static state. Export resolution prefers
 * combase.dll (modern Windows) and falls back to ole32.dll (older systems
 * that export the same functions directly).
 */
class Com
{
public:
	/**
	 * @brief Initializes COM for the calling thread (CoInitializeEx).
	 *
	 * @details Called with COINIT_MULTITHREADED. S_OK and S_FALSE are both
	 * success (S_FALSE means COM was already initialized on this thread);
	 * each success must be balanced by exactly one Uninitialize(). The
	 * caller chains the raw HRESULT under its own failure-site code.
	 *
	 * @return Ok on S_OK/S_FALSE, otherwise Err(Error::Windows(hr)).
	 *
	 * @see CoInitializeEx
	 *      https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex
	 */
	[[nodiscard]] static Result<VOID, Error> Initialize();

	/**
	 * @brief Balances one successful Initialize() (CoUninitialize).
	 * @see CoUninitialize
	 *      https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-couninitialize
	 */
	static VOID Uninitialize();

	/**
	 * @brief Creates a COM object (CoCreateInstance, CLSCTX_INPROC_SERVER).
	 *
	 * @param clsid Class identifier of the object to create.
	 * @param iid Interface identifier requested from the object.
	 *
	 * @return Interface pointer on success, otherwise Err(Error::Windows(hr)).
	 *
	 * @see CoCreateInstance
	 *      https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cocreateinstance
	 */
	[[nodiscard]] static Result<PVOID, Error> CreateInstance(const GUID &clsid, const GUID &iid);

	/**
	 * @brief Frees a COM allocator block (CoTaskMemFree).
	 * @param memory Pointer returned by a COM allocation (e.g. string out-params), or nullptr.
	 * @see CoTaskMemFree
	 *      https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cotaskmemfree
	 */
	static VOID FreeMemory(PVOID memory);

	/**
	 * @brief Clears a PROPVARIANT's payload (PropVariantClear).
	 * @param variant PROPVARIANT to clear (VT_FILETIME/VT_EMPTY clear harmlessly).
	 * @see PropVariantClear
	 *      https://learn.microsoft.com/en-us/windows/win32/api/oleauto/nf-oleauto-propvariantclear
	 */
	static VOID PropVariantClear(PVOID variant);
};
