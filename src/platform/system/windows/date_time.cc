#include "platform/system/date_time.h"
#include "core/types/primitives.h"

/// Address of the shared user data in the process address space
constexpr USIZE MM_SHARED_USER_DATA_VA = 0x7FFE0000;


typedef struct _KSYSTEM_TIME
{
	UINT32 LowPart;
	INT32 High1Time;
	INT32 High2Time;
} KSYSTEM_TIME;


typedef struct _USER_SHARED_DATA
{
	UINT32 TickCountLowDeprecated;       // 0x0
	UINT32 TickCountMultiplier;          // 0x4
	volatile KSYSTEM_TIME InterruptTime; // 0x8
	volatile KSYSTEM_TIME SystemTime;    // 0x14
	volatile KSYSTEM_TIME TimeZoneBias;  // 0x20
} USER_SHARED_DATA, *PUSER_SHARED_DATA;

static FORCE_INLINE PUSER_SHARED_DATA GetUserSharedData()
{
	return (PUSER_SHARED_DATA)MM_SHARED_USER_DATA_VA;
}

static UINT64 ReadKSystemTimeU64(volatile const KSYSTEM_TIME &t)
{
	KSYSTEM_TIME v;
	do
	{
		v.High1Time = t.High1Time;
		v.LowPart = t.LowPart;
	} while (v.High1Time != t.High2Time);

	return (((UINT64)(UINT32)v.High1Time) << 32) | ((UINT64)v.LowPart);
}


static INT64 ReadKSystemTimeS64(volatile const KSYSTEM_TIME &t)
{
	KSYSTEM_TIME v;
	do
	{
		v.High1Time = t.High1Time;
		v.LowPart = t.LowPart;
	} while (v.High1Time != t.High2Time);

	return (INT64)(((UINT64)(UINT32)v.High1Time << 32) | (UINT64)v.LowPart);
}


DateTime DateTime::Now()
{
	DateTime dt;

	volatile USER_SHARED_DATA *usd = GetUserSharedData();

	// 1) UTC time (100ns since 1601-01-01)
	UINT64 utc100ns = ReadKSystemTimeU64(usd->SystemTime);

	// 2) TimeZoneBias (signed 100ns). local = utc - bias
	INT64 bias100ns = ReadKSystemTimeS64(usd->TimeZoneBias);
	INT64 utc100ns_s = (INT64)utc100ns;
	INT64 local100ns_s = utc100ns_s - bias100ns;
	UINT64 local100ns = (UINT64)local100ns_s;

	// 3) Split into date/time
	constexpr UINT64 TICKS_PER_SEC = 10000000ULL;
	constexpr UINT64 TICKS_PER_DAY = 86400ULL * TICKS_PER_SEC;

	UINT64 days = local100ns / TICKS_PER_DAY;
	UINT64 dayTicks = local100ns % TICKS_PER_DAY;

	// Convert 100ns ticks to seconds + nanoseconds for the shared helper
	UINT64 totalSecs = dayTicks / TICKS_PER_SEC;
	UINT64 sub100ns = dayTicks % TICKS_PER_SEC;
	UINT64 subSecNs = sub100ns * UINT64(100u); // 100ns units -> nanoseconds

	DateTime::FromDaysAndTime(dt, days, 1601, totalSecs, subSecNs);
	return dt;
}

UINT64 DateTime::GetMonotonicNanoseconds()
{
	volatile USER_SHARED_DATA *usd = GetUserSharedData();

	// Read InterruptTime (monotonic, unaffected by system clock changes)
	UINT64 interruptTime100ns = ReadKSystemTimeU64(usd->InterruptTime);

	// Convert from 100ns units to nanoseconds
	UINT64 nanoseconds = interruptTime100ns * UINT64(100u);
	return nanoseconds;
}
