#include "platform/fs/windows/wpd.h"
#include "platform/kernel/windows/com.h"
#include "platform/kernel/windows/peb.h"
#include "core/algorithms/djb2.h"
#include "core/string/string.h"
#include "core/memory/memory.h"

// =============================================================================
// GUID/PROPERTYKEY factories — one NOINLINE stack-build factory per constant
// (MakeFsProtocolGuid idiom) so no GUID ever lands in a data section.
// =============================================================================

/// Builds a GUID from its registry-string parts.
static NOINLINE GUID MakeGuid(UINT32 d1, UINT16 d2, UINT16 d3, UINT8 b0, UINT8 b1, UINT8 b2, UINT8 b3, UINT8 b4, UINT8 b5, UINT8 b6, UINT8 b7)
{
	GUID g;
	g.Data1 = d1;
	g.Data2 = d2;
	g.Data3 = d3;
	g.Data4[0] = b0;
	g.Data4[1] = b1;
	g.Data4[2] = b2;
	g.Data4[3] = b3;
	g.Data4[4] = b4;
	g.Data4[5] = b5;
	g.Data4[6] = b6;
	g.Data4[7] = b7;
	return g;
}

/// Builds a PROPERTYKEY from a format id and a property id.
static NOINLINE PROPERTYKEY MakeKey(const GUID &fmtid, UINT32 pid)
{
	PROPERTYKEY k;
	k.Fmtid = fmtid;
	k.Pid = pid;
	return k;
}

// {0AF10CEC-2ECD-4B92-9581-34F6AE0637F3} — CLSID_PortableDeviceManager
static NOINLINE GUID MakeManagerClsid()
{
	return MakeGuid(0x0AF10CEC, 0x2ECD, 0x4B92, 0x95, 0x81, 0x34, 0xF6, 0xAE, 0x06, 0x37, 0xF3);
}

// {A1567595-4C2F-4574-A6FA-ECEF917B9A40} — IID_IPortableDeviceManager
static NOINLINE GUID MakeManagerIid()
{
	return MakeGuid(0xA1567595, 0x4C2F, 0x4574, 0xA6, 0xFA, 0xEC, 0xEF, 0x91, 0x7B, 0x9A, 0x40);
}

// {728A21C5-3D9E-48D7-9810-864848F0F404} — CLSID_PortableDevice
static NOINLINE GUID MakeDeviceClsid()
{
	return MakeGuid(0x728A21C5, 0x3D9E, 0x48D7, 0x98, 0x10, 0x86, 0x48, 0x48, 0xF0, 0xF4, 0x04);
}

// {625E2DF8-6392-4CF0-9AD1-3CFA5F17775C} — IID_IPortableDevice
static NOINLINE GUID MakeDeviceIid()
{
	return MakeGuid(0x625E2DF8, 0x6392, 0x4CF0, 0x9A, 0xD1, 0x3C, 0xFA, 0x5F, 0x17, 0x77, 0x5C);
}

// {6848F6F2-3155-4F86-B6F5-263EEEAB3143} — IID_IPortableDeviceValues
static NOINLINE GUID MakeValuesIid()
{
	return MakeGuid(0x6848F6F2, 0x3155, 0x4F86, 0xB6, 0xF5, 0x26, 0x3E, 0xEE, 0xAB, 0x31, 0x43);
}

// {0C15D503-D017-47CE-9016-7B3F978721CC} — CLSID_PortableDeviceValues
static NOINLINE GUID MakeValuesClsid()
{
	return MakeGuid(0x0C15D503, 0xD017, 0x47CE, 0x90, 0x16, 0x7B, 0x3F, 0x97, 0x87, 0x21, 0xCC);
}

// {DADA2357-E0AD-492E-98DB-DD61C53BA353} — IID_IPortableDeviceKeyCollection
static NOINLINE GUID MakeKeyCollectionIid()
{
	return MakeGuid(0xDADA2357, 0xE0AD, 0x492E, 0x98, 0xDB, 0xDD, 0x61, 0xC5, 0x3B, 0xA3, 0x53);
}

// {DE2D022D-2480-43BE-97F0-D1FA2C9F8F4F} — CLSID_PortableDeviceKeyCollection
static NOINLINE GUID MakeKeyCollectionClsid()
{
	return MakeGuid(0xDE2D022D, 0x2480, 0x43BE, 0x97, 0xF0, 0xD1, 0xFA, 0x2C, 0xF9, 0x8F, 0x4F);
}

// {27E2E392-A111-48E0-AB0C-E17705A05F85} — WPD_CONTENT_TYPE_FOLDER
static NOINLINE GUID MakeContentTypeFolder()
{
	return MakeGuid(0x27E2E392, 0xA111, 0x48E0, 0xAB, 0x0C, 0xE1, 0x77, 0x05, 0xA0, 0x5F, 0x85);
}

// {99ED0160-17FF-4C44-9D98-1D7A6F941921} — WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT (storage category)
static NOINLINE GUID MakeContentTypeFunctionalObject()
{
	return MakeGuid(0x99ED0160, 0x17FF, 0x4C44, 0x9D, 0x98, 0x1D, 0x7A, 0x6F, 0x94, 0x19, 0x21);
}

// {EF6B490D-5CD8-437A-AFFC-DA8B60EE4A3C} — WPD_OBJECT property set fmtid
static NOINLINE GUID MakeObjectFmtid()
{
	return MakeGuid(0xEF6B490D, 0x5CD8, 0x437A, 0xAF, 0xFC, 0xDA, 0x8B, 0x60, 0xEE, 0x4A, 0x3C);
}

// {204D9F0C-2292-4080-9F42-40664E70F859} — WPD_CLIENT property set fmtid
static NOINLINE GUID MakeClientFmtid()
{
	return MakeGuid(0x204D9F0C, 0x2292, 0x4080, 0x9F, 0x42, 0x40, 0x66, 0x4E, 0x70, 0xF8, 0x59);
}

// {E81E79BE-34F0-41BF-B53F-F1A06AE87842} — WPD_RESOURCE fmtid
static NOINLINE GUID MakeResourceFmtid()
{
	return MakeGuid(0xE81E79BE, 0x34F0, 0x41BF, 0xB5, 0x3F, 0xF1, 0xA0, 0x6A, 0xE8, 0x78, 0x42);
}

// {EF6B490D-...},4 — WPD_OBJECT_NAME
static NOINLINE PROPERTYKEY MakeKeyName()
{
	return MakeKey(MakeObjectFmtid(), 4);
}

// {EF6B490D-...},12 — WPD_OBJECT_ORIGINAL_FILE_NAME
static NOINLINE PROPERTYKEY MakeKeyOriginalFileName()
{
	return MakeKey(MakeObjectFmtid(), 12);
}

// {EF6B490D-...},11 — WPD_OBJECT_SIZE
static NOINLINE PROPERTYKEY MakeKeySize()
{
	return MakeKey(MakeObjectFmtid(), 11);
}

// {EF6B490D-...},7 — WPD_OBJECT_CONTENT_TYPE
static NOINLINE PROPERTYKEY MakeKeyContentType()
{
	return MakeKey(MakeObjectFmtid(), 7);
}

// {EF6B490D-...},18 — WPD_OBJECT_DATE_CREATED
static NOINLINE PROPERTYKEY MakeKeyDateCreated()
{
	return MakeKey(MakeObjectFmtid(), 18);
}

// {EF6B490D-...},19 — WPD_OBJECT_DATE_MODIFIED
static NOINLINE PROPERTYKEY MakeKeyDateModified()
{
	return MakeKey(MakeObjectFmtid(), 19);
}

// {E81E79BE-...},0 — WPD_RESOURCE_DEFAULT (the object's data stream)
static NOINLINE PROPERTYKEY MakeKeyResourceDefault()
{
	return MakeKey(MakeResourceFmtid(), 0);
}

// {204D9F0C-...},2 — WPD_CLIENT_NAME
static NOINLINE PROPERTYKEY MakeKeyClientName()
{
	return MakeKey(MakeClientFmtid(), 2);
}

// {204D9F0C-...},3 — WPD_CLIENT_MAJOR_VERSION
static NOINLINE PROPERTYKEY MakeKeyClientMajor()
{
	return MakeKey(MakeClientFmtid(), 3);
}

// {204D9F0C-...},4 — WPD_CLIENT_MINOR_VERSION
static NOINLINE PROPERTYKEY MakeKeyClientMinor()
{
	return MakeKey(MakeClientFmtid(), 4);
}

// {204D9F0C-...},5 — WPD_CLIENT_REVISION
static NOINLINE PROPERTYKEY MakeKeyClientRevision()
{
	return MakeKey(MakeClientFmtid(), 5);
}

// =============================================================================
// State structs (opaque in wpd.h)
// =============================================================================

struct WpdIteratorState
{
	BOOL comInitialized;               ///< TRUE when this state owns a Com::Initialize balance
	IPortableDeviceManager *manager;   ///< Root phase: device manager (null in object phase)
	WCHAR **deviceIds;                 ///< Root phase: PnP device ids (CoTaskMem strings)
	UINT32 deviceCount;                ///< Root phase: number of deviceIds
	UINT32 deviceIndex;                ///< Root phase: next device to emit
	IPortableDevice *device;           ///< Object phase: Open()'d device session
	IPortableDeviceContent *content;   ///< Object phase: content interface
	IPortableDeviceProperties *properties; ///< Object phase: property reader
	IPortableDeviceKeyCollection *keys;    ///< Object phase: fetched property set
	IEnumPortableDeviceObjectIDs *enumerator; ///< Object phase: child id cursor
	BOOL atDeviceRoot;                 ///< Object phase: enumerating the device root (its storages)
};

struct WpdStreamState
{
	BOOL comInitialized;               ///< TRUE when this state owns a Com::Initialize balance
	IPortableDevice *device;           ///< Open()'d device session
	IPortableDeviceContent *content;   ///< Content interface
	IPortableDeviceProperties *properties; ///< Property reader
	IPortableDeviceResources *resources;   ///< Resource/stream interface
	WCHAR *objectId;                   ///< Resolved object id (CoTaskMem; null = DEVICE root)
	IStream *stream;                   ///< The open data stream
	UINT64 size;                       ///< Object size in bytes (WPD_OBJECT_SIZE)
	UINT64 position;                   ///< Current absolute stream offset (tracked)
	BOOL seekable;                     ///< FALSE for sequential-only device streams (Seek unsupported)
};

// Release helper: calls Release() through the vtable and nulls the pointer.
template <typename T>
static FORCE_INLINE VOID SafeRelease(T *&interfacePtr)
{
	if (interfacePtr != nullptr)
	{
		(VOID)interfacePtr->lpVtbl->Release(interfacePtr);
		interfacePtr = nullptr;
	}
}

static FORCE_INLINE BOOL Failed(HRESULT hr)
{
	return hr < 0;
}

/// Full GUID equality (Data1-3 plus all 8 Data4 bytes).
static FORCE_INLINE BOOL SameGuid(const GUID &a, const GUID &b)
{
	return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 && Memory::Compare(a.Data4, b.Data4, 8) == 0;
}

// =============================================================================
// Internal open/resolve pipeline shared by enumeration and streaming
// =============================================================================

/// Everything acquired while opening a device and resolving a pseudo-path.
/// Fields are handed off to the iterator/stream states; FreeSession only
/// releases what is still set.
struct WpdDeviceSession
{
	BOOL comInitialized;               ///< Owns a Com::Initialize balance until hand-off
	WCHAR **deviceIds;                 ///< PnP ids from GetDevices (freed after Open)
	UINT32 deviceCount;                ///< Number of deviceIds
	IPortableDeviceManager *manager;   ///< Released once the device is open
	IPortableDevice *device;           ///< Open()'d device
	IPortableDeviceContent *content;   ///< Content interface
	IPortableDeviceProperties *properties; ///< Property reader
	WCHAR *objectId;                   ///< Resolved target object (DEVICE root = nullptr)
};

/// Releases everything still set in the session and the COM init it owns.
static VOID FreeSession(WpdDeviceSession *session)
{
	if (session == nullptr)
		return;
	SafeRelease(session->properties);
	SafeRelease(session->content);
	if (session->device != nullptr)
	{
		(VOID)session->device->lpVtbl->Close(session->device);
		SafeRelease(session->device);
	}
	SafeRelease(session->manager);
	if (session->deviceIds != nullptr)
	{
		for (UINT32 i = 0; i < session->deviceCount; i++)
			Com::FreeMemory(session->deviceIds[i]);
		delete[] session->deviceIds;
		session->deviceIds = nullptr;
	}
	if (session->objectId != nullptr)
	{
		Com::FreeMemory(session->objectId);
		session->objectId = nullptr;
	}
	if (session->comInitialized)
	{
		Com::Uninitialize();
		session->comInitialized = false;
	}
	delete session;
}

/// Fetches one string-valued property into a fresh CoTaskMem string.
static Result<WCHAR *, Error> GetStringValue(IPortableDeviceValues *values, const PROPERTYKEY &key)
{
	WCHAR *out = nullptr;
	HRESULT hr = values->lpVtbl->GetStringValue(values, &key, &out);
	if (Failed(hr))
	{
		Com::FreeMemory(out);
		return Result<WCHAR *, Error>::Err(Error::Windows((UINT32)hr));
	}
	return Result<WCHAR *, Error>::Ok(out);
}

/// Reads an object's display name: WPD_OBJECT_NAME, falling back to
/// WPD_OBJECT_ORIGINAL_FILE_NAME, then the object id itself.
static VOID ReadObjectName(IPortableDeviceValues *values, PCWCHAR objectId, Span<WCHAR> out)
{
	out[0] = L'\0';
	for (int attempt = 0; attempt < 2; attempt++)
	{
		auto name = GetStringValue(values, attempt == 0 ? MakeKeyName() : MakeKeyOriginalFileName());
		if (name)
		{
			if (name.Value() != nullptr && name.Value()[0] != L'\0')
			{
				StringUtils::Copy(out, Span<const WCHAR>(name.Value(), StringUtils::Length(name.Value())));
				Com::FreeMemory(name.Value());
				return;
			}
			Com::FreeMemory(name.Value());
		}
	}
	if (objectId != nullptr)
		StringUtils::Copy(out, Span<const WCHAR>(objectId, StringUtils::Length(objectId)));
}

/// Reads a FILETIME-valued property. The out variant is cleared unconditionally:
/// VT_FILETIME owns no memory, but a schema-violating driver returning a
/// string/blob for a date property must not leak.
static UINT64 ReadDateValue(IPortableDeviceValues *values, const PROPERTYKEY &key)
{
	PROPVARIANT_WPD prop;
	Memory::Zero(&prop, sizeof(prop));
	UINT64 out = 0;
	if (values->lpVtbl->GetValue(values, &key, &prop) == 0 && prop.vt == VT_FILETIME)
		out = ((UINT64)prop.filetime.dwHighDateTime << 32) | prop.filetime.dwLowDateTime;
	Com::PropVariantClear(&prop);
	return out;
}

/// Builds the property key collection fetched for every object read.
static Result<IPortableDeviceKeyCollection *, Error> CreatePropertyKeys()
{
	auto collection = Com::CreateInstance(MakeKeyCollectionClsid(), MakeKeyCollectionIid());
	if (!collection)
		return Result<IPortableDeviceKeyCollection *, Error>::Err(collection.Error(), Error::Fs_OpenFailed);

	auto *keys = (IPortableDeviceKeyCollection *)collection.Value();
	GUID objectFmtid = MakeObjectFmtid();
	const PROPERTYKEY keysToAdd[] = {
		MakeKey(objectFmtid, 4),  // WPD_OBJECT_NAME
		MakeKey(objectFmtid, 12), // WPD_OBJECT_ORIGINAL_FILE_NAME
		MakeKey(objectFmtid, 11), // WPD_OBJECT_SIZE
		MakeKey(objectFmtid, 7),  // WPD_OBJECT_CONTENT_TYPE
		MakeKey(objectFmtid, 18), // WPD_OBJECT_DATE_CREATED
		MakeKey(objectFmtid, 19), // WPD_OBJECT_DATE_MODIFIED
	};
	for (USIZE i = 0; i < sizeof(keysToAdd) / sizeof(keysToAdd[0]); i++)
	{
		HRESULT hr = keys->lpVtbl->Add(keys, &keysToAdd[i]);
		if (Failed(hr))
		{
			SafeRelease(keys);
			return Result<IPortableDeviceKeyCollection *, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_OpenFailed);
		}
	}
	return Result<IPortableDeviceKeyCollection *, Error>::Ok(keys);
}

/// Fetches the requested properties of one object.
static Result<IPortableDeviceValues *, Error> GetObjectValues(IPortableDeviceProperties *properties, IPortableDeviceKeyCollection *keys, PCWCHAR objectId)
{
	IPortableDeviceValues *values = nullptr;
	HRESULT hr = properties->lpVtbl->GetValues(properties, objectId, keys, &values);
	if (Failed(hr) || values == nullptr)
	{
		SafeRelease(values);
		return Result<IPortableDeviceValues *, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_ReadFailed);
	}
	return Result<IPortableDeviceValues *, Error>::Ok(values);
}

/// Opens COM + the device manager and pulls the PnP device id list.
static Result<WpdDeviceSession *, Error> CreateSession()
{
	auto comInit = Com::Initialize();
	if (!comInit)
		return Result<WpdDeviceSession *, Error>::Err(comInit.Error());

	auto *session = new WpdDeviceSession;
	if (session == nullptr)
	{
		Com::Uninitialize();
		return Result<WpdDeviceSession *, Error>::Err(Error::Fs_OpenFailed);
	}
	Memory::Zero(session, sizeof(WpdDeviceSession));
	session->comInitialized = true;

	auto manager = Com::CreateInstance(MakeManagerClsid(), MakeManagerIid());
	if (!manager)
	{
		FreeSession(session);
		return Result<WpdDeviceSession *, Error>::Err(manager.Error(), Error::Fs_OpenFailed);
	}
	session->manager = (IPortableDeviceManager *)manager.Value();

	UINT32 count = 0;
	HRESULT hr = session->manager->lpVtbl->GetDevices(session->manager, nullptr, &count);
	if (Failed(hr))
	{
		FreeSession(session);
		return Result<WpdDeviceSession *, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_OpenFailed);
	}
	if (count == 0)
		return Result<WpdDeviceSession *, Error>::Ok(session); // valid, empty device list

	session->deviceIds = new WCHAR *[count];
	if (session->deviceIds == nullptr)
	{
		FreeSession(session);
		return Result<WpdDeviceSession *, Error>::Err(Error::Fs_OpenFailed);
	}
	// Count set before the call: a failure after a partial fill still frees
	// every id FreeSession walks (the two-call contract promises none).
	session->deviceCount = count;
	hr = session->manager->lpVtbl->GetDevices(session->manager, session->deviceIds, &count);
	if (Failed(hr))
	{
		FreeSession(session);
		return Result<WpdDeviceSession *, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_OpenFailed);
	}
	return Result<WpdDeviceSession *, Error>::Ok(session);
}

/// Finds the device whose id hashes to the root-listing token. The token is
/// Djb2::Hash of the id — already case-folding, so casing never matters.
static const WCHAR *MatchDevice(WpdDeviceSession *session, UINT64 token)
{
	for (UINT32 i = 0; i < session->deviceCount; i++)
	{
		if (Djb2::Hash(session->deviceIds[i]) == token)
			return session->deviceIds[i];
	}
	return nullptr;
}

/// Walks one path segment among the children of parentObjectId (exact name
/// match first, then case-insensitive; duplicate names resolve to the first
/// match). Returns the matched child id (CoTaskMem, caller frees).
static Result<WCHAR *, Error> WalkSegment(IPortableDeviceContent *content, IPortableDeviceProperties *properties, IPortableDeviceKeyCollection *keys, PCWCHAR parentObjectId, const WCHAR *segment, USIZE segmentLen)
{
	WCHAR name[256];
	const WCHAR *parent = parentObjectId != nullptr ? parentObjectId : L"DEVICE";
	IEnumPortableDeviceObjectIDs *enumerator = nullptr;
	HRESULT hr = content->lpVtbl->EnumObjects(content, 0, parent, nullptr, &enumerator);
	if (Failed(hr) || enumerator == nullptr)
		return Result<WCHAR *, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_PathResolveFailed);

	WCHAR *exact = nullptr;
	WCHAR *insensitive = nullptr;
	WCHAR *childId = nullptr;
	UINT32 fetched = 0;
	while ((hr = enumerator->lpVtbl->Next(enumerator, 1, &childId, &fetched)) >= 0 && fetched > 0 && childId != nullptr)
	{
		auto values = GetObjectValues(properties, keys, childId);
		if (values)
		{
			ReadObjectName(values.Value(), childId, Span<WCHAR>(name));
			if (StringUtils::Compare(Span<const WCHAR>(name, StringUtils::Length(name)), Span<const WCHAR>(segment, segmentLen)))
			{
				Com::FreeMemory(insensitive);
				insensitive = nullptr;
				exact = childId;
				childId = nullptr;
				SafeRelease(values.Value());
				break; // exact wins: no device round-trips draining the rest
			}
			if (insensitive == nullptr && StringUtils::Compare(Span<const WCHAR>(name, StringUtils::Length(name)), Span<const WCHAR>(segment, segmentLen), true))
				insensitive = childId;
			else
				Com::FreeMemory(childId);
			SafeRelease(values.Value());
		}
		else
		{
			Com::FreeMemory(childId); // unreadable child: skip it
		}
		childId = nullptr;
	}
	Com::FreeMemory(childId);
	SafeRelease(enumerator);

	if (exact != nullptr)
		return Result<WCHAR *, Error>::Ok(exact);
	if (insensitive != nullptr)
		return Result<WCHAR *, Error>::Ok(insensitive);
	return Result<WCHAR *, Error>::Err(Error::Windows(0x80070002u), Error::Fs_PathResolveFailed); // ERROR_FILE_NOT_FOUND
}

/// Opens the device behind a pseudo-path and resolves the full subpath to an
/// object id. Missing device → 0x80070651 (ERROR_DEVICE_REMOVED); missing
/// object → 0x80070002 (ERROR_FILE_NOT_FOUND) — both classified Fs_* by the
/// beacon's ClassifyError.
static Result<WpdDeviceSession *, Error> OpenDevicePath(PCWCHAR path)
{
	UINT64 token = 0;
	const WCHAR *subpath = nullptr;
	USIZE subpathLen = 0;
	if (!WPD::TryParsePath(path, token, subpath, subpathLen))
		return Result<WpdDeviceSession *, Error>::Err(Error::Fs_PathResolveFailed);

	auto sessionResult = CreateSession();
	if (!sessionResult)
		return sessionResult;
	WpdDeviceSession *session = sessionResult.Value();

	const WCHAR *matchedId = MatchDevice(session, token);
	if (matchedId == nullptr)
	{
		FreeSession(session);
		return Result<WpdDeviceSession *, Error>::Err(Error::Windows(0x80070651u), Error::Fs_OpenFailed);
	}

	auto deviceResult = Com::CreateInstance(MakeDeviceClsid(), MakeDeviceIid());
	if (!deviceResult)
	{
		FreeSession(session);
		return Result<WpdDeviceSession *, Error>::Err(deviceResult.Error(), Error::Fs_OpenFailed);
	}
	session->device = (IPortableDevice *)deviceResult.Value();

	// Neutral client identification — WPD rejects Open() without one.
	auto clientInfo = Com::CreateInstance(MakeValuesClsid(), MakeValuesIid());
	if (!clientInfo)
	{
		FreeSession(session);
		return Result<WpdDeviceSession *, Error>::Err(clientInfo.Error(), Error::Fs_OpenFailed);
	}
	auto *values = (IPortableDeviceValues *)clientInfo.Value();
	PROPERTYKEY keyClientName = MakeKeyClientName();
	PROPERTYKEY keyClientMajor = MakeKeyClientMajor();
	PROPERTYKEY keyClientMinor = MakeKeyClientMinor();
	PROPERTYKEY keyClientRevision = MakeKeyClientRevision();
	values->lpVtbl->SetStringValue(values, &keyClientName, L"pird");
	values->lpVtbl->SetUnsignedIntegerValue(values, &keyClientMajor, 1);
	values->lpVtbl->SetUnsignedIntegerValue(values, &keyClientMinor, 0);
	values->lpVtbl->SetUnsignedIntegerValue(values, &keyClientRevision, 0);
	HRESULT hr = session->device->lpVtbl->Open(session->device, matchedId, values);
	SafeRelease(values);
	if (Failed(hr))
	{
		FreeSession(session);
		return Result<WpdDeviceSession *, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_OpenFailed);
	}

	// The device id list is no longer needed once Open() has the matched id.
	for (UINT32 i = 0; i < session->deviceCount; i++)
		Com::FreeMemory(session->deviceIds[i]);
	delete[] session->deviceIds;
	session->deviceIds = nullptr;
	session->deviceCount = 0;
	SafeRelease(session->manager);

	// Content/Properties come off the opened device (not CoCreated directly).
	hr = session->device->lpVtbl->Content(session->device, &session->content);
	if (Failed(hr) || session->content == nullptr)
	{
		FreeSession(session);
		return Result<WpdDeviceSession *, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_OpenFailed);
	}
	hr = session->content->lpVtbl->Properties(session->content, &session->properties);
	if (Failed(hr) || session->properties == nullptr)
	{
		FreeSession(session);
		return Result<WpdDeviceSession *, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_OpenFailed);
	}

	auto keys = CreatePropertyKeys();
	if (!keys)
	{
		FreeSession(session);
		return Result<WpdDeviceSession *, Error>::Err(keys.Error(), Error::Fs_OpenFailed);
	}

	// Walk the object path segment by segment from the device root.
	WCHAR *current = nullptr;
	USIZE offset = 0;
	while (offset < subpathLen)
	{
		USIZE start = offset;
		while (offset < subpathLen && subpath[offset] != L'\\')
			offset++;
		USIZE segmentLen = offset - start;
		if (segmentLen == 0)
			break; // trailing (or doubled) separator: current is the target
		auto next = WalkSegment(session->content, session->properties, keys.Value(), current, subpath + start, segmentLen);
		Com::FreeMemory(current);
		if (!next)
		{
			SafeRelease(keys.Value());
			FreeSession(session);
			return Result<WpdDeviceSession *, Error>::Err(next.Error(), Error::Fs_PathResolveFailed);
		}
		current = next.Value();
		if (offset < subpathLen)
			offset++; // skip the separator
	}
	SafeRelease(keys.Value());
	session->objectId = current;
	return Result<WpdDeviceSession *, Error>::Ok(session);
}

// =============================================================================
// Root listing helpers
// =============================================================================

/// Maps a friendly-name character to its wire-safe form (drive-root grammar
/// characters and control chars become '_').
static FORCE_INLINE WCHAR SanitizeNameChar(WCHAR c)
{
	switch (c)
	{
	case L'\\':
	case L'/':
	case L':':
	case L'*':
	case L'?':
	case L'"':
	case L'<':
	case L'>':
	case L'|':
		return L'_';
	default:
		return c < 0x20 ? L'_' : c;
	}
}

/// Formats `::mtp-<16 lowercase hex>[-<sanitized friendly>]` (total ≤ 255).
static USIZE FormatDeviceName(UINT64 token, PCWCHAR friendly, Span<WCHAR> out)
{
	USIZE length = 0;
	const WCHAR prefix[] = L"::mtp-";
	for (USIZE i = 0; prefix[i] != L'\0'; i++)
		out[length++] = prefix[i];
	for (int i = 15; i >= 0; i--)
	{
		UINT8 nibble = (UINT8)((token >> (i * 4)) & 0xF);
		out[length++] = (WCHAR)(nibble < 10 ? L'0' + nibble : L'a' + nibble - 10);
	}
	if (friendly != nullptr && friendly[0] != L'\0')
	{
		out[length++] = L'-';
		for (USIZE i = 0; friendly[i] != L'\0' && length + 1 < 255; i++)
			out[length++] = SanitizeNameChar(friendly[i]);
	}
	out[length] = L'\0';
	return length;
}

/// Two-call GetDeviceFriendlyName; FALSE when the device names nothing or the
/// name does not fit (the pseudo-root still lists, just without a label).
static BOOL FetchFriendlyName(IPortableDeviceManager *manager, PCWCHAR deviceId, Span<WCHAR> out)
{
	UINT32 size = 0;
	HRESULT hr = manager->lpVtbl->GetDeviceFriendlyName(manager, deviceId, nullptr, &size);
	if (Failed(hr) || size == 0)
		return false;
	if (size > out.Size())
		size = (UINT32)out.Size();
	hr = manager->lpVtbl->GetDeviceFriendlyName(manager, deviceId, out.Data(), &size);
	if (Failed(hr) || out.Data()[0] == L'\0')
		return false;
	out.Data()[size > 0 ? size - 1 : 0] = L'\0'; // size returned includes the terminator
	return true;
}

/// Emits the next root-listing device entry (manager phase).
static Result<BOOL, Error> NextRootEntry(WpdIteratorState *state, DirectoryEntry &out)
{
	if (state->deviceIndex >= state->deviceCount)
		return Result<BOOL, Error>::Ok(false);

	PCWCHAR deviceId = state->deviceIds[state->deviceIndex++];
	UINT64 token = Djb2::Hash(deviceId);

	WCHAR friendly[128];
	const DirectoryEntry empty{};
	out = empty;
	FormatDeviceName(token, FetchFriendlyName(state->manager, deviceId, Span<WCHAR>(friendly)) ? friendly : nullptr, Span<WCHAR>(out.Name));

	out.IsDirectory = true;
	out.IsDrive = true;
	out.Type = DRIVE_REMOVABLE;
	out.VolumeSerial = token;
	return Result<BOOL, Error>::Ok(true);
}

/// Emits the next object entry (object phase).
static Result<BOOL, Error> NextObjectEntry(WpdIteratorState *state, DirectoryEntry &out)
{
	if (state->enumerator == nullptr)
		return Result<BOOL, Error>::Ok(false);

	WCHAR *objectId = nullptr;
	UINT32 fetched = 0;
	HRESULT hr = state->enumerator->lpVtbl->Next(state->enumerator, 1, &objectId, &fetched);
	if (Failed(hr))
		return Result<BOOL, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_ReadFailed);
	if (fetched == 0 || objectId == nullptr)
	{
		Com::FreeMemory(objectId);
		return Result<BOOL, Error>::Ok(false);
	}

	auto values = GetObjectValues(state->properties, state->keys, objectId);
	if (!values)
	{
		// One unreadable object must not end the listing: emit it with the
		// object id as its name and zeroed metadata (the same loss-averse
		// policy WalkSegment uses for unreadable children).
		const DirectoryEntry empty{};
		out = empty;
		StringUtils::Copy(Span<WCHAR>(out.Name), Span<const WCHAR>(objectId, StringUtils::Length(objectId)));
		out.IsDirectory = state->atDeviceRoot;
		Com::FreeMemory(objectId);
		return Result<BOOL, Error>::Ok(true);
	}

	const DirectoryEntry empty{};
	out = empty;
	ReadObjectName(values.Value(), objectId, Span<WCHAR>(out.Name));

	// Storages are FUNCTIONAL_OBJECT children of the device root, and some
	// devices omit their content type: root children are always directories.
	if (state->atDeviceRoot)
		out.IsDirectory = true;
	else
	{
		GUID contentType;
		Memory::Zero(&contentType, sizeof(contentType));
		PROPERTYKEY keyContentType = MakeKeyContentType();
		if (values.Value()->lpVtbl->GetGuidValue(values.Value(), &keyContentType, &contentType) == 0)
			out.IsDirectory = SameGuid(contentType, MakeContentTypeFolder()) || SameGuid(contentType, MakeContentTypeFunctionalObject());
	}

	UINT64 size = 0;
	PROPERTYKEY keySize = MakeKeySize();
	if (values.Value()->lpVtbl->GetUnsignedLargeIntegerValue(values.Value(), &keySize, &size) == 0)
		out.Size = size;
	out.CreationTime = ReadDateValue(values.Value(), MakeKeyDateCreated());
	out.LastModifiedTime = ReadDateValue(values.Value(), MakeKeyDateModified());

	SafeRelease(values.Value());
	Com::FreeMemory(objectId);
	return Result<BOOL, Error>::Ok(true);
}

// =============================================================================
// WPD public API
// =============================================================================

BOOL WPD::TryParsePath(PCWCHAR path, UINT64 &token, const WCHAR *&subpath, USIZE &subpathLen)
{
	if (path == nullptr || path[0] != L':' || path[1] != L':')
		return false;
	if (path[2] != L'm' || path[3] != L't' || path[4] != L'p' || path[5] != L'-')
		return false;

	UINT64 value = 0;
	for (USIZE i = 6; i < 22; i++)
	{
		WCHAR c = path[i];
		UINT8 nibble;
		if (c >= L'0' && c <= L'9')
			nibble = (UINT8)(c - L'0');
		else if (c >= L'a' && c <= L'f')
			nibble = (UINT8)(c - L'a' + 10);
		else if (c >= L'A' && c <= L'F')
			nibble = (UINT8)(c - L'A' + 10);
		else
			return false;
		value = (value << 4) | nibble;
	}
	token = value;

	if (path[22] == L'\0')
	{
		subpath = path + 22;
		subpathLen = 0;
		return true;
	}
	if (path[22] == L'\\')
	{
		subpath = path + 23;
		subpathLen = StringUtils::Length(path) - 23;
		return true;
	}
	if (path[22] == L'-')
	{
		// The friendly name is display-only; skip to the first separator.
		for (USIZE i = 23;; i++)
		{
			if (path[i] == L'\0')
			{
				subpath = path + i;
				subpathLen = 0;
				return true;
			}
			if (path[i] == L'\\')
			{
				subpath = path + i + 1;
				subpathLen = StringUtils::Length(path) - i - 1;
				return true;
			}
		}
	}
	return false;
}

Result<WpdIteratorState *, Error> WPD::BeginRootEnumeration()
{
	auto sessionResult = CreateSession();
	if (!sessionResult)
		return Result<WpdIteratorState *, Error>::Err(sessionResult.Error(), Error::Fs_OpenFailed);
	WpdDeviceSession *session = sessionResult.Value();

	auto *state = new WpdIteratorState;
	if (state == nullptr)
	{
		FreeSession(session);
		return Result<WpdIteratorState *, Error>::Err(Error::Fs_OpenFailed);
	}
	Memory::Zero(state, sizeof(WpdIteratorState));

	// Hand off the manager phase + device id list to the iterator state.
	state->comInitialized = session->comInitialized;
	session->comInitialized = false;
	state->manager = session->manager;
	session->manager = nullptr;
	state->deviceIds = session->deviceIds;
	session->deviceIds = nullptr;
	state->deviceCount = session->deviceCount;
	session->deviceCount = 0;
	state->deviceIndex = 0;
	FreeSession(session);
	return Result<WpdIteratorState *, Error>::Ok(state);
}

Result<WpdIteratorState *, Error> WPD::BeginObjectEnumeration(PCWCHAR path)
{
	auto sessionResult = OpenDevicePath(path);
	if (!sessionResult)
		return Result<WpdIteratorState *, Error>::Err(sessionResult.Error(), Error::Fs_OpenFailed);
	WpdDeviceSession *session = sessionResult.Value();

	auto keys = CreatePropertyKeys();
	if (!keys)
	{
		FreeSession(session);
		return Result<WpdIteratorState *, Error>::Err(keys.Error(), Error::Fs_OpenFailed);
	}

	// Enumerate the children of the resolved folder (device root lists its
	// storages). EnumObjects takes a const parent; DEVICE when unresolved.
	IEnumPortableDeviceObjectIDs *enumerator = nullptr;
	HRESULT hr = session->content->lpVtbl->EnumObjects(session->content, 0, session->objectId != nullptr ? session->objectId : L"DEVICE", nullptr, &enumerator);
	if (Failed(hr) || enumerator == nullptr)
	{
		SafeRelease(keys.Value());
		FreeSession(session);
		return Result<WpdIteratorState *, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_OpenFailed);
	}

	auto *state = new WpdIteratorState;
	if (state == nullptr)
	{
		SafeRelease(enumerator);
		SafeRelease(keys.Value());
		FreeSession(session);
		return Result<WpdIteratorState *, Error>::Err(Error::Fs_OpenFailed);
	}
	Memory::Zero(state, sizeof(WpdIteratorState));
	state->comInitialized = session->comInitialized;
	session->comInitialized = false;
	state->device = session->device;
	session->device = nullptr;
	state->content = session->content;
	session->content = nullptr;
	state->properties = session->properties;
	session->properties = nullptr;
	state->keys = keys.Value();
	state->enumerator = enumerator;
	state->atDeviceRoot = session->objectId == nullptr;
	FreeSession(session);
	return Result<WpdIteratorState *, Error>::Ok(state);
}

Result<BOOL, Error> WPD::NextEntry(WpdIteratorState *state, DirectoryEntry &out)
{
	if (state == nullptr)
		return Result<BOOL, Error>::Err(Error::Fs_ReadFailed);
	if (state->manager != nullptr)
		return NextRootEntry(state, out);
	return NextObjectEntry(state, out);
}

VOID WPD::EndEnumeration(WpdIteratorState *state)
{
	if (state == nullptr)
		return;
	SafeRelease(state->enumerator);
	SafeRelease(state->keys);
	SafeRelease(state->properties);
	SafeRelease(state->content);
	if (state->device != nullptr)
	{
		(VOID)state->device->lpVtbl->Close(state->device);
		SafeRelease(state->device);
	}
	if (state->deviceIds != nullptr)
	{
		for (UINT32 i = 0; i < state->deviceCount; i++)
			Com::FreeMemory(state->deviceIds[i]);
		delete[] state->deviceIds;
		state->deviceIds = nullptr;
		state->deviceCount = 0;
	}
	SafeRelease(state->manager);
	if (state->comInitialized)
	{
		Com::Uninitialize();
		state->comInitialized = false;
	}
	delete state;
}

Result<WpdStreamState *, Error> WPD::OpenStream(PCWCHAR path, UINT64 &sizeOut)
{
	auto sessionResult = OpenDevicePath(path);
	if (!sessionResult)
		return Result<WpdStreamState *, Error>::Err(sessionResult.Error(), Error::Fs_OpenFailed);
	WpdDeviceSession *session = sessionResult.Value();

	auto keys = CreatePropertyKeys();
	if (!keys)
	{
		FreeSession(session);
		return Result<WpdStreamState *, Error>::Err(keys.Error(), Error::Fs_OpenFailed);
	}

	auto *state = new WpdStreamState;
	if (state == nullptr)
	{
		SafeRelease(keys.Value());
		FreeSession(session);
		return Result<WpdStreamState *, Error>::Err(Error::Fs_OpenFailed);
	}
	Memory::Zero(state, sizeof(WpdStreamState));

	// Stat the size via properties before handing the interfaces to the state.
	const WCHAR *objectId = session->objectId != nullptr ? session->objectId : L"DEVICE";
	UINT64 size = 0;
	{
		auto values = GetObjectValues(session->properties, keys.Value(), objectId);
		if (!values)
		{
			SafeRelease(keys.Value());
			delete state;
			FreeSession(session);
			return Result<WpdStreamState *, Error>::Err(values.Error(), Error::Fs_OpenFailed);
		}
		PROPERTYKEY keySize = MakeKeySize();
		(VOID)values.Value()->lpVtbl->GetUnsignedLargeIntegerValue(values.Value(), &keySize, &size);
		SafeRelease(values.Value());
	}

	IPortableDeviceResources *resources = nullptr;
	HRESULT hr = session->content->lpVtbl->Transfer(session->content, &resources);
	if (Failed(hr) || resources == nullptr)
	{
		SafeRelease(keys.Value());
		delete state;
		FreeSession(session);
		return Result<WpdStreamState *, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_OpenFailed);
	}
	UINT32 optimal = 0;
	PROPERTYKEY keyResource = MakeKeyResourceDefault();
	hr = resources->lpVtbl->GetStream(resources, objectId, &keyResource, STGM_READ, &optimal, &state->stream);
	if (Failed(hr) || state->stream == nullptr)
	{
		SafeRelease(state->stream); // defensive: out-param must be null on failure
		SafeRelease(resources);
		SafeRelease(keys.Value());
		delete state;
		FreeSession(session);
		return Result<WpdStreamState *, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_OpenFailed);
	}
	SafeRelease(keys.Value());

	// Probe seekability once: many MTP drivers expose sequential-only resource
	// streams (Seek -> E_NOTIMPL); the stream still reads fine from offset 0.
	LARGE_INTEGER zero;
	zero.QuadPart = 0;
	ULARGE_INTEGER probedPosition;
	Memory::Zero(&probedPosition, sizeof(probedPosition));
	state->seekable = !Failed(state->stream->lpVtbl->Seek(state->stream, zero, STREAM_SEEK_SET, &probedPosition));

	state->comInitialized = session->comInitialized;
	session->comInitialized = false;
	state->device = session->device;
	session->device = nullptr;
	state->content = session->content;
	session->content = nullptr;
	state->properties = session->properties;
	session->properties = nullptr;
	state->resources = resources;
	state->objectId = session->objectId;
	session->objectId = nullptr;
	state->size = size;
	state->position = 0;
	sizeOut = size;
	FreeSession(session);
	return Result<WpdStreamState *, Error>::Ok(state);
}

Result<USIZE, Error> WPD::StreamRead(WpdStreamState *state, Span<UINT8> buffer)
{
	if (state == nullptr || state->stream == nullptr)
		return Result<USIZE, Error>::Err(Error::Fs_ReadFailed);
	UINT32 toRead = buffer.Size() > (USIZE)0xFFFFFFFFu ? 0xFFFFFFFFu : (UINT32)buffer.Size();
	UINT32 fetched = 0;
	HRESULT hr = state->stream->lpVtbl->Read(state->stream, buffer.Data(), toRead, &fetched);
	if (Failed(hr))
		return Result<USIZE, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_ReadFailed);
	state->position += fetched;
	return Result<USIZE, Error>::Ok((USIZE)fetched);
}

/// Sequential-only device streams: re-opens the resource stream (a fresh
/// stream starts at 0) and discards forward to the target offset. Hitting EOF
/// first means the offset is beyond the end of the object.
static Result<VOID, Error> DiscardToOffset(WpdStreamState *state, UINT64 target)
{
	SafeRelease(state->stream);
	UINT32 optimal = 0;
	PROPERTYKEY keyResource = MakeKeyResourceDefault();
	HRESULT hr = state->resources->lpVtbl->GetStream(state->resources, state->objectId != nullptr ? state->objectId : L"DEVICE", &keyResource, STGM_READ, &optimal, &state->stream);
	if (Failed(hr) || state->stream == nullptr)
	{
		SafeRelease(state->stream); // defensive: out-param must be null on failure
		return Result<VOID, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_SeekFailed);
	}
	state->position = 0;
	UINT8 discard[8192];
	while (state->position < target)
	{
		UINT64 remaining = target - state->position;
		UINT32 chunk = remaining > sizeof(discard) ? (UINT32)sizeof(discard) : (UINT32)remaining;
		UINT32 fetched = 0;
		hr = state->stream->lpVtbl->Read(state->stream, discard, chunk, &fetched);
		if (Failed(hr))
			return Result<VOID, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_SeekFailed);
		if (fetched == 0)
			return Result<VOID, Error>::Err(Error::Windows(0x80070026u), Error::Fs_SeekFailed); // ERROR_HANDLE_EOF
		state->position += fetched;
	}
	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> WPD::StreamSeek(WpdStreamState *state, USIZE absoluteOffset)
{
	if (state == nullptr || state->stream == nullptr)
		return Result<VOID, Error>::Err(Error::Fs_SeekFailed);
	if ((UINT64)absoluteOffset == state->position)
		return Result<VOID, Error>::Ok(); // SetOffset(0)-then-read makes no COM call
	if (state->seekable)
	{
		LARGE_INTEGER move;
		move.QuadPart = (INT64)absoluteOffset;
		ULARGE_INTEGER newPosition;
		Memory::Zero(&newPosition, sizeof(newPosition));
		HRESULT hr = state->stream->lpVtbl->Seek(state->stream, move, STREAM_SEEK_SET, &newPosition);
		if (Failed(hr))
			return Result<VOID, Error>::Err(Error::Windows((UINT32)hr), Error::Fs_SeekFailed);
		state->position = newPosition.QuadPart;
		return Result<VOID, Error>::Ok();
	}
	return DiscardToOffset(state, (UINT64)absoluteOffset);
}

Result<USIZE, Error> WPD::StreamTell(WpdStreamState *state)
{
	if (state == nullptr)
		return Result<USIZE, Error>::Err(Error::Fs_SeekFailed);
	return Result<USIZE, Error>::Ok((USIZE)state->position);
}

VOID WPD::CloseStream(WpdStreamState *state)
{
	if (state == nullptr)
		return;
	SafeRelease(state->stream);
	SafeRelease(state->resources);
	SafeRelease(state->properties);
	SafeRelease(state->content);
	if (state->device != nullptr)
	{
		(VOID)state->device->lpVtbl->Close(state->device);
		SafeRelease(state->device);
	}
	if (state->objectId != nullptr)
	{
		Com::FreeMemory(state->objectId);
		state->objectId = nullptr;
	}
	if (state->comInitialized)
	{
		Com::Uninitialize();
		state->comInitialized = false;
	}
	delete state;
}
