/**
 * @file wpd.h
 * @brief Windows Portable Device (WPD) Read-Only File-System Bridge
 *
 * @details Exposes MTP/portable devices (phones, cameras) as pseudo-roots of
 * the platform file-system layer. Every interface is hand-declared C-style
 * (plain struct + vtable pointer, the UEFI protocol idiom) in full Windows SDK
 * declaration order, so no SDK headers or import libraries are needed. Export
 * resolution goes through Com (combase.dll / ole32.dll, per call).
 *
 * Pseudo-path grammar: `::mtp-<16 hex token>[-<friendly name>][\<subpath>]`.
 * The token is Djb2::Hash of the PnP device id (case-folding hash, so the
 * emitted lowercase hex and the device id always re-match). Friendly names are
 * display-only and never parsed. Object subpaths split on `\` only.
 *
 * Read-only scope: enumeration and content reads only; write-mode opens on a
 * WPD path are rejected. HRESULT failures are returned raw in
 * Error::Windows(...) — the beacon classifies them into Fs_* cause codes.
 *
 * @see Windows Portable Devices architecture
 *      https://learn.microsoft.com/en-us/windows/win32/portabledevices/portable-device-programming-guide
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/error.h"
#include "core/types/result.h"
#include "core/types/span.h"
#include "platform/kernel/windows/com.h"
#include "platform/kernel/windows/windows_types.h"
#include "platform/fs/directory_entry.h"

#define STGM_READ 0 ///< Open for reading only (COM stream access mode)
#define VT_FILETIME 64 ///< PROPVARIANT type carrying a FILETIME (WPD date properties)
#define STREAM_SEEK_SET 0 ///< IStream seek origin: absolute from start

/** @brief COM status code (SDK HRESULT: 32-bit, bit 31 = failure severity). */
typedef INT32 HRESULT;
/** @brief SDK reference-count return type (32-bit unsigned long on Windows). */
typedef UINT32 ULONG;

/** @brief 64-bit unsigned integer as a low/high pair (SDK ULARGE_INTEGER). */
typedef union _ULARGE_INTEGER
{
	struct
	{
		UINT32 LowPart;  ///< Low 32 bits
		UINT32 HighPart; ///< High 32 bits
	};
	UINT64 QuadPart; ///< Full 64-bit value
} ULARGE_INTEGER, *PULARGE_INTEGER;

/** @brief Win32 file timestamp: 100-nanosecond intervals since 1601-01-01 UTC. */
typedef struct _FILETIME
{
	UINT32 dwLowDateTime;  ///< Low 32 bits of the 64-bit timestamp
	UINT32 dwHighDateTime; ///< High 32 bits of the 64-bit timestamp
} FILETIME, *PFILETIME;

/** @brief Property identifier: a GUID format id plus a property id within it. */
typedef struct _PROPERTYKEY
{
	GUID Fmtid; ///< Format identifier (property set)
	UINT32 Pid; ///< Property id within the set
} PROPERTYKEY, *PPROPERTYKEY;

/**
 * @brief Minimal PROPVARIANT: VARTYPE tag plus a full-size payload union.
 * @details The payload is oversized to the SDK union (16 bytes) because COM
 * writes the whole structure. Only VT_FILETIME payloads are consumed.
 */
typedef struct _PROPVARIANT_WPD
{
	UINT16 vt;        ///< VARTYPE tag (VT_FILETIME for WPD date properties)
	UINT16 reserved[3]; ///< SDK padding words
	union
	{
		FILETIME filetime; ///< VT_FILETIME payload (WPD_OBJECT_DATE_*)
		UINT8 payload[16]; ///< Full SDK union size — COM writes all of it
	};
} PROPVARIANT_WPD;

// --- Hand-declared WPD/COM interfaces (opaque structs, full SDK vtables) ---

struct IPortableDeviceManager;
struct IPortableDevice;
struct IPortableDeviceContent;
struct IEnumPortableDeviceObjectIDs;
struct IPortableDeviceProperties;
struct IPortableDeviceValues;
struct IPortableDeviceKeyCollection;
struct IPortableDeviceResources;
struct IStream;
struct STATSTG;

/** @brief IPortableDeviceManager vtable — SDK slot order (portabledeviceapi.h). */
struct IPortableDeviceManagerVtbl
{
	HRESULT (STDCALL *QueryInterface)(IPortableDeviceManager *, const GUID *, PVOID *);
	ULONG (STDCALL *AddRef)(IPortableDeviceManager *);
	ULONG (STDCALL *Release)(IPortableDeviceManager *);
	HRESULT (STDCALL *GetDevices)(IPortableDeviceManager *, WCHAR **, UINT32 *);
	HRESULT (STDCALL *RefreshDeviceList)(IPortableDeviceManager *);
	HRESULT (STDCALL *GetDeviceFriendlyName)(IPortableDeviceManager *, const WCHAR *, WCHAR *, UINT32 *);
	HRESULT (STDCALL *GetDeviceDescription)(IPortableDeviceManager *, const WCHAR *, WCHAR *, UINT32 *);
	HRESULT (STDCALL *GetDeviceManufacturer)(IPortableDeviceManager *, const WCHAR *, WCHAR *, UINT32 *);
	HRESULT (STDCALL *GetDeviceProperty)(IPortableDeviceManager *, const WCHAR *, const WCHAR *, UINT8 *, UINT32 *, UINT32 *);
	HRESULT (STDCALL *GetPrivateDevices)(IPortableDeviceManager *, WCHAR **, UINT32 *);
};

/** @brief IPortableDeviceManager — enumerates portable devices. */
struct IPortableDeviceManager
{
	const IPortableDeviceManagerVtbl *lpVtbl; ///< Interface vtable (SDK slot order)
};

/** @brief IPortableDevice vtable — SDK slot order (portabledeviceapi.h). */
struct IPortableDeviceVtbl
{
	HRESULT (STDCALL *QueryInterface)(IPortableDevice *, const GUID *, PVOID *);
	ULONG (STDCALL *AddRef)(IPortableDevice *);
	ULONG (STDCALL *Release)(IPortableDevice *);
	HRESULT (STDCALL *Open)(IPortableDevice *, const WCHAR *, IPortableDeviceValues *);
	HRESULT (STDCALL *SendCommand)(IPortableDevice *, UINT32, IPortableDeviceValues *, IPortableDeviceValues **);
	HRESULT (STDCALL *Content)(IPortableDevice *, IPortableDeviceContent **);
	HRESULT (STDCALL *Capabilities)(IPortableDevice *, PVOID *);
	HRESULT (STDCALL *Cancel)(IPortableDevice *);
	HRESULT (STDCALL *Close)(IPortableDevice *);
	HRESULT (STDCALL *Advise)(IPortableDevice *, UINT32, PVOID, IPortableDeviceValues *, WCHAR **);
	HRESULT (STDCALL *Unadvise)(IPortableDevice *, const WCHAR *);
	HRESULT (STDCALL *GetPnPDeviceID)(IPortableDevice *, WCHAR **);
};

/** @brief IPortableDevice — an Open()'d session with one device. */
struct IPortableDevice
{
	const IPortableDeviceVtbl *lpVtbl; ///< Interface vtable (SDK slot order)
};

/** @brief IPortableDeviceContent vtable — SDK slot order (portabledeviceapi.h). */
struct IPortableDeviceContentVtbl
{
	HRESULT (STDCALL *QueryInterface)(IPortableDeviceContent *, const GUID *, PVOID *);
	ULONG (STDCALL *AddRef)(IPortableDeviceContent *);
	ULONG (STDCALL *Release)(IPortableDeviceContent *);
	HRESULT (STDCALL *EnumObjects)(IPortableDeviceContent *, UINT32, const WCHAR *, IPortableDeviceValues *, IEnumPortableDeviceObjectIDs **);
	HRESULT (STDCALL *Properties)(IPortableDeviceContent *, IPortableDeviceProperties **);
	HRESULT (STDCALL *Transfer)(IPortableDeviceContent *, IPortableDeviceResources **);
	HRESULT (STDCALL *CreateObjectWithPropertiesOnly)(IPortableDeviceContent *, IPortableDeviceValues *, WCHAR **);
	HRESULT (STDCALL *CreateObjectWithPropertiesAndData)(IPortableDeviceContent *, IPortableDeviceValues *, IStream **, UINT32 *, WCHAR **);
	HRESULT (STDCALL *Delete)(IPortableDeviceContent *, UINT32, PVOID, PVOID);
	HRESULT (STDCALL *GetObjectIDsFromPersistentUniqueIDs)(IPortableDeviceContent *, PVOID, PVOID);
	HRESULT (STDCALL *Cancel)(IPortableDeviceContent *);
	HRESULT (STDCALL *Move)(IPortableDeviceContent *, PVOID, const WCHAR *, PVOID);
	HRESULT (STDCALL *Copy)(IPortableDeviceContent *, PVOID, const WCHAR *, PVOID);
};

/** @brief IPortableDeviceContent — object enumeration/properties/transfer entry point. */
struct IPortableDeviceContent
{
	const IPortableDeviceContentVtbl *lpVtbl; ///< Interface vtable (SDK slot order)
};

/** @brief IEnumPortableDeviceObjectIDs vtable — SDK slot order (portabledeviceapi.h). */
struct IEnumPortableDeviceObjectIDsVtbl
{
	HRESULT (STDCALL *QueryInterface)(IEnumPortableDeviceObjectIDs *, const GUID *, PVOID *);
	ULONG (STDCALL *AddRef)(IEnumPortableDeviceObjectIDs *);
	ULONG (STDCALL *Release)(IEnumPortableDeviceObjectIDs *);
	HRESULT (STDCALL *Next)(IEnumPortableDeviceObjectIDs *, UINT32, WCHAR **, UINT32 *);
	HRESULT (STDCALL *Skip)(IEnumPortableDeviceObjectIDs *, UINT32);
	HRESULT (STDCALL *Reset)(IEnumPortableDeviceObjectIDs *);
	HRESULT (STDCALL *Clone)(IEnumPortableDeviceObjectIDs *, IEnumPortableDeviceObjectIDs **);
};

/** @brief IEnumPortableDeviceObjectIDs — child object id cursor of one folder. */
struct IEnumPortableDeviceObjectIDs
{
	const IEnumPortableDeviceObjectIDsVtbl *lpVtbl; ///< Interface vtable (SDK slot order)
};

/** @brief IPortableDeviceProperties vtable — SDK slot order (portabledeviceapi.h). */
struct IPortableDevicePropertiesVtbl
{
	HRESULT (STDCALL *QueryInterface)(IPortableDeviceProperties *, const GUID *, PVOID *);
	ULONG (STDCALL *AddRef)(IPortableDeviceProperties *);
	ULONG (STDCALL *Release)(IPortableDeviceProperties *);
	HRESULT (STDCALL *GetSupportedProperties)(IPortableDeviceProperties *, const WCHAR *, IPortableDeviceKeyCollection **);
	HRESULT (STDCALL *GetPropertyAttributes)(IPortableDeviceProperties *, const WCHAR *, const PROPERTYKEY *, IPortableDeviceValues **);
	HRESULT (STDCALL *GetValues)(IPortableDeviceProperties *, const WCHAR *, IPortableDeviceKeyCollection *, IPortableDeviceValues **);
	HRESULT (STDCALL *SetValues)(IPortableDeviceProperties *, const WCHAR *, IPortableDeviceValues *, IPortableDeviceValues **);
	HRESULT (STDCALL *Delete)(IPortableDeviceProperties *, const WCHAR *, IPortableDeviceKeyCollection *);
	HRESULT (STDCALL *Cancel)(IPortableDeviceProperties *);
};

/** @brief IPortableDeviceProperties — bulk property reads per object. */
struct IPortableDeviceProperties
{
	const IPortableDevicePropertiesVtbl *lpVtbl; ///< Interface vtable (SDK slot order)
};

/** @brief IPortableDeviceValues vtable — full SDK slot order (portabledevicetypes.h). */
struct IPortableDeviceValuesVtbl
{
	HRESULT (STDCALL *QueryInterface)(IPortableDeviceValues *, const GUID *, PVOID *);
	ULONG (STDCALL *AddRef)(IPortableDeviceValues *);
	ULONG (STDCALL *Release)(IPortableDeviceValues *);
	HRESULT (STDCALL *GetCount)(IPortableDeviceValues *, UINT32 *);
	HRESULT (STDCALL *GetAt)(IPortableDeviceValues *, UINT32, PROPERTYKEY *, PROPVARIANT_WPD *);
	HRESULT (STDCALL *SetValue)(IPortableDeviceValues *, const PROPERTYKEY *, const PROPVARIANT_WPD *);
	HRESULT (STDCALL *GetValue)(IPortableDeviceValues *, const PROPERTYKEY *, PROPVARIANT_WPD *);
	HRESULT (STDCALL *SetStringValue)(IPortableDeviceValues *, const PROPERTYKEY *, const WCHAR *);
	HRESULT (STDCALL *GetStringValue)(IPortableDeviceValues *, const PROPERTYKEY *, WCHAR **);
	HRESULT (STDCALL *SetUnsignedIntegerValue)(IPortableDeviceValues *, const PROPERTYKEY *, UINT32);
	HRESULT (STDCALL *GetUnsignedIntegerValue)(IPortableDeviceValues *, const PROPERTYKEY *, UINT32 *);
	HRESULT (STDCALL *SetSignedIntegerValue)(IPortableDeviceValues *, const PROPERTYKEY *, INT32);
	HRESULT (STDCALL *GetSignedIntegerValue)(IPortableDeviceValues *, const PROPERTYKEY *, INT32 *);
	HRESULT (STDCALL *SetUnsignedLargeIntegerValue)(IPortableDeviceValues *, const PROPERTYKEY *, UINT64);
	HRESULT (STDCALL *GetUnsignedLargeIntegerValue)(IPortableDeviceValues *, const PROPERTYKEY *, UINT64 *);
	HRESULT (STDCALL *SetSignedLargeIntegerValue)(IPortableDeviceValues *, const PROPERTYKEY *, INT64);
	HRESULT (STDCALL *GetSignedLargeIntegerValue)(IPortableDeviceValues *, const PROPERTYKEY *, INT64 *);
	HRESULT (STDCALL *SetFloatValue)(IPortableDeviceValues *, const PROPERTYKEY *, float);
	HRESULT (STDCALL *GetFloatValue)(IPortableDeviceValues *, const PROPERTYKEY *, float *);
	HRESULT (STDCALL *SetErrorValue)(IPortableDeviceValues *, const PROPERTYKEY *, UINT32);
	HRESULT (STDCALL *GetErrorValue)(IPortableDeviceValues *, const PROPERTYKEY *, UINT32 *);
	HRESULT (STDCALL *SetKeyValue)(IPortableDeviceValues *, const PROPERTYKEY *, const PROPERTYKEY *);
	HRESULT (STDCALL *GetKeyValue)(IPortableDeviceValues *, const PROPERTYKEY *, PROPERTYKEY *);
	HRESULT (STDCALL *SetBoolValue)(IPortableDeviceValues *, const PROPERTYKEY *, BOOL);
	HRESULT (STDCALL *GetBoolValue)(IPortableDeviceValues *, const PROPERTYKEY *, BOOL *);
	HRESULT (STDCALL *SetIUnknownValue)(IPortableDeviceValues *, const PROPERTYKEY *, PVOID);
	HRESULT (STDCALL *GetIUnknownValue)(IPortableDeviceValues *, const PROPERTYKEY *, PVOID *);
	HRESULT (STDCALL *SetGuidValue)(IPortableDeviceValues *, const PROPERTYKEY *, const GUID *);
	HRESULT (STDCALL *GetGuidValue)(IPortableDeviceValues *, const PROPERTYKEY *, GUID *);
	HRESULT (STDCALL *SetBufferValue)(IPortableDeviceValues *, const PROPERTYKEY *, UINT8 *, UINT32);
	HRESULT (STDCALL *GetBufferValue)(IPortableDeviceValues *, const PROPERTYKEY *, UINT8 **, UINT32 *);
	HRESULT (STDCALL *SetIPortableDeviceValuesValue)(IPortableDeviceValues *, const PROPERTYKEY *, IPortableDeviceValues *);
	HRESULT (STDCALL *GetIPortableDeviceValuesValue)(IPortableDeviceValues *, const PROPERTYKEY *, IPortableDeviceValues **);
	HRESULT (STDCALL *SetIPortableDevicePropVariantCollectionValue)(IPortableDeviceValues *, const PROPERTYKEY *, PVOID);
	HRESULT (STDCALL *GetIPortableDevicePropVariantCollectionValue)(IPortableDeviceValues *, const PROPERTYKEY *, PVOID *);
	HRESULT (STDCALL *SetIPortableDeviceKeyCollectionValue)(IPortableDeviceValues *, const PROPERTYKEY *, IPortableDeviceKeyCollection *);
	HRESULT (STDCALL *GetIPortableDeviceKeyCollectionValue)(IPortableDeviceValues *, const PROPERTYKEY *, IPortableDeviceKeyCollection **);
	HRESULT (STDCALL *SetIPortableDeviceValuesCollectionValue)(IPortableDeviceValues *, const PROPERTYKEY *, PVOID);
	HRESULT (STDCALL *GetIPortableDeviceValuesCollectionValue)(IPortableDeviceValues *, const PROPERTYKEY *, PVOID *);
	HRESULT (STDCALL *RemoveValue)(IPortableDeviceValues *, const PROPERTYKEY *);
	HRESULT (STDCALL *CopyValuesFromPropertyStore)(IPortableDeviceValues *, PVOID);
	HRESULT (STDCALL *CopyValuesToPropertyStore)(IPortableDeviceValues *, PVOID);
	HRESULT (STDCALL *Clear)(IPortableDeviceValues *);
};

/** @brief IPortableDeviceValues — typed key/value bag (client info, object properties). */
struct IPortableDeviceValues
{
	const IPortableDeviceValuesVtbl *lpVtbl; ///< Interface vtable (SDK slot order)
};

/** @brief IPortableDeviceKeyCollection vtable — SDK slot order (portabledevicetypes.h). */
struct IPortableDeviceKeyCollectionVtbl
{
	HRESULT (STDCALL *QueryInterface)(IPortableDeviceKeyCollection *, const GUID *, PVOID *);
	ULONG (STDCALL *AddRef)(IPortableDeviceKeyCollection *);
	ULONG (STDCALL *Release)(IPortableDeviceKeyCollection *);
	HRESULT (STDCALL *GetCount)(IPortableDeviceKeyCollection *, UINT32 *);
	HRESULT (STDCALL *GetAt)(IPortableDeviceKeyCollection *, UINT32, PROPERTYKEY *);
	HRESULT (STDCALL *Add)(IPortableDeviceKeyCollection *, const PROPERTYKEY *);
	HRESULT (STDCALL *Clear)(IPortableDeviceKeyCollection *);
	HRESULT (STDCALL *RemoveAt)(IPortableDeviceKeyCollection *, UINT32);
};

/** @brief IPortableDeviceKeyCollection — the property set fetched per object. */
struct IPortableDeviceKeyCollection
{
	const IPortableDeviceKeyCollectionVtbl *lpVtbl; ///< Interface vtable (SDK slot order)
};

/** @brief IPortableDeviceResources vtable — SDK slot order (portabledeviceapi.h). */
struct IPortableDeviceResourcesVtbl
{
	HRESULT (STDCALL *QueryInterface)(IPortableDeviceResources *, const GUID *, PVOID *);
	ULONG (STDCALL *AddRef)(IPortableDeviceResources *);
	ULONG (STDCALL *Release)(IPortableDeviceResources *);
	HRESULT (STDCALL *GetSupportedResources)(IPortableDeviceResources *, const WCHAR *, IPortableDeviceKeyCollection **);
	HRESULT (STDCALL *GetResourceAttributes)(IPortableDeviceResources *, const WCHAR *, const PROPERTYKEY *, IPortableDeviceValues **);
	HRESULT (STDCALL *GetStream)(IPortableDeviceResources *, const WCHAR *, const PROPERTYKEY *, UINT32, UINT32 *, IStream **);
	HRESULT (STDCALL *Delete)(IPortableDeviceResources *, const WCHAR *, IPortableDeviceKeyCollection *);
	HRESULT (STDCALL *Cancel)(IPortableDeviceResources *);
	HRESULT (STDCALL *CreateResource)(IPortableDeviceResources *, IPortableDeviceValues *, IStream **, UINT32 *, WCHAR **);
};

/** @brief IPortableDeviceResources — data-stream access to object resources. */
struct IPortableDeviceResources
{
	const IPortableDeviceResourcesVtbl *lpVtbl; ///< Interface vtable (SDK slot order)
};

/** @brief IStream vtable — full ISequentialStream + IStream SDK slot order (objidl.h). */
struct IStreamVtbl
{
	HRESULT (STDCALL *QueryInterface)(IStream *, const GUID *, PVOID *);
	ULONG (STDCALL *AddRef)(IStream *);
	ULONG (STDCALL *Release)(IStream *);
	HRESULT (STDCALL *Read)(IStream *, VOID *, UINT32, UINT32 *);
	HRESULT (STDCALL *Write)(IStream *, const VOID *, UINT32, UINT32 *);
	HRESULT (STDCALL *Seek)(IStream *, LARGE_INTEGER, UINT32, ULARGE_INTEGER *);
	HRESULT (STDCALL *SetSize)(IStream *, ULARGE_INTEGER);
	HRESULT (STDCALL *CopyTo)(IStream *, IStream *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *);
	HRESULT (STDCALL *Commit)(IStream *, UINT32);
	HRESULT (STDCALL *Revert)(IStream *);
	HRESULT (STDCALL *LockRegion)(IStream *, ULARGE_INTEGER, ULARGE_INTEGER, UINT32);
	HRESULT (STDCALL *UnlockRegion)(IStream *, ULARGE_INTEGER, ULARGE_INTEGER, UINT32);
	HRESULT (STDCALL *Stat)(IStream *, STATSTG *, UINT32);
	HRESULT (STDCALL *Clone)(IStream *, IStream **);
};

/** @brief IStream — the readable data stream of one WPD object resource. */
struct IStream
{
	const IStreamVtbl *lpVtbl; ///< Interface vtable (SDK slot order)
};

/// Opaque per-iterator WPD state (root device listing or object enumeration).
struct WpdIteratorState;
/// Opaque per-stream WPD state (one open object data stream).
struct WpdStreamState;

/**
 * @brief Portable-device (MTP) file-system service.
 *
 * @details Bridges the WPD COM API into the platform fs layer. Iterator and
 * stream states are opaque structs owned by the caller as PVOID fields of
 * DirectoryIterator/File; every begin/open is balanced by exactly one
 * end/close, and COM init/uninit is balanced per state object.
 */
class WPD
{
public:
	/**
	 * @brief Parses a portable-device pseudo-path.
	 *
	 * @details Grammar: `::mtp-<16 hex>[-<friendly>][\<subpath>]`. On a match
	 * the 64-bit device token is decoded and @p subpath points into @p path
	 * at the first character after the separator (empty for the device root).
	 * Returns FALSE for anything not in this grammar so the caller falls
	 * through to the NT layer unchanged.
	 *
	 * @param path Null-terminated wide path to classify.
	 * @param token Receives the 64-bit device token on match.
	 * @param subpath Receives the pointer to the object subpath (may be empty).
	 * @param subpathLen Receives the subpath length in WCHARs.
	 * @return TRUE if the path is a portable-device pseudo-path.
	 */
	static BOOL TryParsePath(PCWCHAR path, UINT64 &token, const WCHAR *&subpath, USIZE &subpathLen);

	/**
	 * @brief Begins the root listing device phase (devices only, none opened).
	 *
	 * @details CoInitializeEx + IPortableDeviceManager::GetDevices; friendly
	 * names are fetched per entry inside NextEntry(). Each emitted entry is
	 * `::mtp-<token>[-<friendly>]` with IsDrive=TRUE, Type=removable,
	 * VolumeSerial=token.
	 *
	 * @return Opaque iterator state, or an Error (COM unavailable, no manager).
	 */
	[[nodiscard]] static Result<WpdIteratorState *, Error> BeginRootEnumeration();

	/**
	 * @brief Begins enumeration of a device folder addressed by pseudo-path.
	 *
	 * @details Parses the path, re-enumerates GetDevices and hash-matches the
	 * token, opens the device with neutral client info, then walks the object
	 * subpath segment by segment (exact match, then case-insensitive;
	 * duplicates resolve to the first match). An empty subpath enumerates the
	 * device root (its storages). Missing device: 0x80070651; missing object:
	 * 0x80070002 — both wrapped under the iterator failure-site code.
	 *
	 * @param path Pseudo-path as emitted by the root listing.
	 * @return Opaque iterator state, or an Error.
	 */
	[[nodiscard]] static Result<WpdIteratorState *, Error> BeginObjectEnumeration(PCWCHAR path);

	/**
	 * @brief Advances a WPD enumeration by one entry.
	 *
	 * @param state State from BeginRootEnumeration/BeginObjectEnumeration.
	 * @param out Receives the next entry (root phase: device pseudo-root;
	 *            object phase: file/folder with size, dates, folder flag).
	 *
	 * @return Ok(TRUE) with an entry, Ok(FALSE) at clean end, or an Error.
	 */
	[[nodiscard]] static Result<BOOL, Error> NextEntry(WpdIteratorState *state, DirectoryEntry &out);

	/**
	 * @brief Releases all WPD/COM resources of an iterator state.
	 * @details Safe with partially-initialized states; the pointer is invalid
	 * after the call.
	 */
	static VOID EndEnumeration(WpdIteratorState *state);

	/**
	 * @brief Opens a readable stream for the object addressed by pseudo-path.
	 *
	 * @details Resolves the path exactly like BeginObjectEnumeration, queries
	 * WPD_OBJECT_SIZE, then IPortableDeviceResources::GetStream on
	 * WPD_RESOURCE_DEFAULT (STGM_READ). The stream starts at offset 0.
	 * Seekability is probed once with a Seek(0); drivers that reject Seek
	 * (sequential-only MTP resource streams) still open — position is then
	 * tracked internally and backward seeks re-open + discard (see StreamSeek).
	 *
	 * @param path Pseudo-path to a file object.
	 * @param sizeOut Receives the object size in bytes.
	 * @return Opaque stream state, or an Error.
	 */
	[[nodiscard]] static Result<WpdStreamState *, Error> OpenStream(PCWCHAR path, UINT64 &sizeOut);

	/**
	 * @brief Reads sequentially from an open WPD stream at its position.
	 * @details Advances the internally tracked position by the bytes read.
	 * @param state State from OpenStream.
	 * @param buffer Destination buffer (clamped to UINT32 per COM Read).
	 * @return Bytes actually read (0 = end of stream), or an Error.
	 */
	[[nodiscard]] static Result<USIZE, Error> StreamRead(WpdStreamState *state, Span<UINT8> buffer);

	/**
	 * @brief Positions the stream at an absolute byte offset.
	 * @details A no-op when already at @p absoluteOffset (the common
	 * SetOffset(0)-then-read pattern makes no COM call). Seekable streams seek
	 * directly; sequential-only device streams re-acquire the resource stream
	 * from offset 0 and discard forward, failing with 0x80070026
	 * (ERROR_HANDLE_EOF) when the offset is beyond the end of the object.
	 * @param state State from OpenStream.
	 * @param absoluteOffset Byte offset from the start of the object.
	 * @return Ok on success, or an Error.
	 */
	[[nodiscard]] static Result<VOID, Error> StreamSeek(WpdStreamState *state, USIZE absoluteOffset);

	/**
	 * @brief Returns the current absolute stream position.
	 * @details Reads the internally tracked position — never a COM call.
	 * @param state State from OpenStream.
	 * @return Position in bytes, or an Error.
	 */
	[[nodiscard]] static Result<USIZE, Error> StreamTell(WpdStreamState *state);

	/**
	 * @brief Releases all WPD/COM resources of a stream state.
	 * @details Closes the device session and balances Com::Initialize; the
	 * pointer is invalid after the call.
	 */
	static VOID CloseStream(WpdStreamState *state);
};
