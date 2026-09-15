/**
 * @file date_time.h
 * @brief Date and time utilities
 *
 * @details Provides the DateTime class for system clock access and date/time
 * formatting. Includes DateTime::Now() for retrieving the current wall-clock
 * time via platform syscalls, monotonic timestamp access for entropy and
 * timing, and constexpr formatting methods that produce fixed-size strings
 * (time-only, date-only, and full date-time) without heap allocation.
 * Also provides calendar helper methods such as leap year detection and
 * epoch-to-date conversion shared across platform implementations.
 */
#pragma once

#include "core/core.h"
/**
 * @brief Fixed-size, stack-allocated character buffer for formatted date/time strings.
 * @details Provides a non-heap, non-.rdata string container used by DateTime formatting
 * methods. Supports explicit conversion to raw character pointers and element indexing.
 * @tparam TChar Character type (CHAR or WCHAR).
 * @tparam N Buffer capacity including the null terminator.
 */
template <TCHAR TChar, USIZE N>
struct FixedString
{
private:
	TChar data[N]{}; ///< Fixed-size character array, zero-initialized

public:
	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}

	/**
	 * @brief Converts to a const character pointer.
	 * @return Pointer to the internal character buffer.
	 */
	explicit constexpr operator const TChar *() const { return data; }

	/**
	 * @brief Converts to a mutable character pointer.
	 * @return Pointer to the internal character buffer.
	 */
	explicit constexpr operator TChar *() { return data; }

	/**
	 * @brief Accesses a character by index.
	 * @param i Zero-based index into the buffer.
	 * @return Reference to the character at position i.
	 */
	constexpr TChar &operator[](USIZE i) { return data[i]; }
};

/// Type alias for a time-only string: "HH:MM:SS\0" (9 characters).
template <TCHAR TChar>
using TimeOnlyString = FixedString<TChar, 9>;

/// Type alias for a date-only string: "YYYY-MM-DD\0" (11 characters).
template <TCHAR TChar>
using DateOnlyString = FixedString<TChar, 11>;

/// Type alias for a full date-time string: "YYYY-MM-DD HH:MM:SS\0" (20 characters).
template <TCHAR TChar>
using DateTimeString = FixedString<TChar, 20>;

/**
 * @brief Date and time representation with formatting and calendar utilities.
 * @details Provides system clock access via Now(), monotonic timestamps via
 * GetMonotonicNanoseconds(), and constexpr formatting methods that produce
 * fixed-size strings without heap allocation. Also includes calendar helpers
 * (leap year detection, epoch-to-date conversion) shared across platforms.
 */
class DateTime
{
private:
	/// Writes a 2-digit decimal value into dst.
	template <TCHAR TChar>
	static constexpr VOID Put2(Span<TChar> dst, UINT32 v)
	{
		dst[0] = (TChar)('0' + ((v / 10u) % 10u));
		dst[1] = (TChar)('0' + (v % 10u));
	}
	/// Writes a 4-digit decimal value into dst.
	template <TCHAR TChar>
	static constexpr VOID Put4(Span<TChar> dst, UINT32 v)
	{
		dst[0] = (TChar)('0' + ((v / 1000u) % 10u));
		dst[1] = (TChar)('0' + ((v / 100u) % 10u));
		dst[2] = (TChar)('0' + ((v / 10u) % 10u));
		dst[3] = (TChar)('0' + (v % 10u));
	}

public:
	UINT64 Years = 0;        
	UINT32 Months = 0;       
	UINT32 Days = 0;         
	UINT32 Hours = 0;        
	UINT32 Minutes = 0;     
	UINT32 Seconds = 0;      
	UINT64 Milliseconds = 0; 
	UINT64 Microseconds = 0; 
	UINT64 Nanoseconds = 0;

	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}

	/**
	 * @brief Formats the time portion as "HH:MM:SS".
	 * @tparam TChar Character type (CHAR or WCHAR).
	 * @return A FixedString containing the formatted time.
	 */
	template <TCHAR TChar>
	constexpr TimeOnlyString<TChar> ToTimeOnlyString() const
	{
		TimeOnlyString<TChar> out{};

		Put2<TChar>(Span<TChar>(&out[0], 2), (UINT32)Hours);
		out[2] = (TChar)':';
		Put2<TChar>(Span<TChar>(&out[3], 2), (UINT32)Minutes);
		out[5] = (TChar)':';
		Put2<TChar>(Span<TChar>(&out[6], 2), (UINT32)Seconds);

		out[8] = (TChar)0;
		return out;
	}

	/**
	 * @brief Formats the date portion as "YYYY-MM-DD".
	 * @tparam TChar Character type (CHAR or WCHAR).
	 * @return A FixedString containing the formatted date.
	 */
	template <TCHAR TChar>
	constexpr DateOnlyString<TChar> ToDateOnlyString() const
	{
		DateOnlyString<TChar> out{};

		Put4<TChar>(Span<TChar>(&out[0], 4), (UINT32)Years);
		out[4] = (TChar)'-';
		Put2<TChar>(Span<TChar>(&out[5], 2), (UINT32)Months);
		out[7] = (TChar)'-';
		Put2<TChar>(Span<TChar>(&out[8], 2), (UINT32)Days);

		out[10] = (TChar)0;
		return out;
	}

	/**
	 * @brief Formats the full date and time as "YYYY-MM-DD HH:MM:SS".
	 * @tparam TChar Character type (CHAR or WCHAR).
	 * @return A FixedString containing the formatted date-time.
	 */
	template <TCHAR TChar>
	constexpr DateTimeString<TChar> ToDateTimeString() const
	{
		DateTimeString<TChar> out{};

		// Date
		Put4<TChar>(Span<TChar>(&out[0], 4), (UINT32)Years);
		out[4] = (TChar)'-';
		Put2<TChar>(Span<TChar>(&out[5], 2), (UINT32)Months);
		out[7] = (TChar)'-';
		Put2<TChar>(Span<TChar>(&out[8], 2), (UINT32)Days);
		out[10] = (TChar)' ';

		// Time
		Put2<TChar>(Span<TChar>(&out[11], 2), (UINT32)Hours);
		out[13] = (TChar)':';
		Put2<TChar>(Span<TChar>(&out[14], 2), (UINT32)Minutes);
		out[16] = (TChar)':';
		Put2<TChar>(Span<TChar>(&out[17], 2), (UINT32)Seconds);

		out[19] = (TChar)0;
		return out;
	}

	/// Alias for ToTimeOnlyString() (backward compatibility).
	template <TCHAR TChar>
	constexpr TimeOnlyString<TChar> ToTimeString() const
	{
		return ToTimeOnlyString<TChar>();
	}

	/// Alias for ToDateOnlyString() (backward compatibility).
	template <TCHAR TChar>
	constexpr DateOnlyString<TChar> ToDateString() const
	{
		return ToDateOnlyString<TChar>();
	}

	/**
	 * @brief Retrieves the current wall-clock date and time from the system.
	 * @return A populated DateTime struct with all components set.
	 */
	static DateTime Now();

	/**
	 * @brief Returns a monotonic timestamp in nanoseconds.
	 * @details Useful for entropy seeding and elapsed-time measurement.
	 * The epoch is platform-defined and not comparable across reboots.
	 * @return Monotonic nanosecond count.
	 */
	static UINT64 GetMonotonicNanoseconds();

	/**
	 * @brief Determines whether a given year is a leap year.
	 * @param year The year to check.
	 * @return TRUE if the year is a leap year, FALSE otherwise.
	 */
	static constexpr FORCE_INLINE BOOL IsLeapYear(UINT64 year) noexcept
	{
		return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
	}

	/**
	 * @brief Returns the number of days in a given month.
	 * @param month Month number (1-indexed: 1=January, 12=December).
	 * @param isLeapYear TRUE if the year is a leap year.
	 * @return Number of days in the specified month.
	 */
	static constexpr FORCE_INLINE UINT32 GetDaysInMonth(UINT32 month, BOOL isLeapYear) noexcept
	{
		// Days in each month (non-leap year): Jan=31, Feb=28, Mar=31, Apr=30, May=31, Jun=30, Jul=31, Aug=31, Sep=30, Oct=31, Nov=30, Dec=31
		// Using a computed approach to avoid .rdata dependency
		if (month == 2)
			return isLeapYear ? 29 : 28;
		if (month == 4 || month == 6 || month == 9 || month == 11)
			return 30;
		return 31;
	}

	/**
	 * @brief Converts a zero-based day-of-year to month and day-of-month.
	 * @param dayOfYear Zero-based day within the year (0 = January 1).
	 * @param year The year (used for leap year calculation).
	 * @param outMonth Output: month number (1-12).
	 * @param outDay Output: day of month (1-31).
	 */
	static constexpr VOID DaysToMonthDay(UINT64 dayOfYear, UINT64 year, UINT32& outMonth, UINT32& outDay) noexcept
	{
		BOOL isLeap = IsLeapYear(year);
		UINT32 month = 1;
		UINT64 remainingDays = dayOfYear;

		while (month <= 12)
		{
			UINT32 daysInMonth = GetDaysInMonth(month, isLeap);
			if (remainingDays < daysInMonth)
				break;
			remainingDays -= daysInMonth;
			month++;
		}

		outMonth = month;
		outDay = (UINT32)remainingDays + 1; // Days are 1-indexed
	}

	/**
	 * @brief Populates a DateTime from days-since-epoch and time-of-day components.
	 * @details Shared helper used by platform-specific Now() implementations to convert
	 * raw epoch values into a fully populated DateTime struct.
	 * @param dt Output DateTime to populate.
	 * @param days Number of days elapsed since baseYear.
	 * @param baseYear The epoch year (e.g., 1601 for Windows FILETIME, 1970 for Unix).
	 * @param timeOfDaySeconds Seconds elapsed within the current day (0-86399).
	 * @param subSecondNanoseconds Nanosecond remainder within the current second.
	 */
	static constexpr VOID FromDaysAndTime(DateTime& dt, UINT64 days, UINT64 baseYear,
							   UINT64 timeOfDaySeconds, UINT64 subSecondNanoseconds) noexcept
	{
		// Fast-forward through years
		UINT64 year = baseYear;
		while (true)
		{
			UINT32 daysInYear = IsLeapYear(year) ? 366u : 365u;
			if (days >= daysInYear)
			{
				days -= daysInYear;
				year++;
			}
			else
				break;
		}

		UINT32 month, day;
		DaysToMonthDay(days, year, month, day);

		dt.Years = year;
		dt.Months = month;
		dt.Days = day;

		dt.Hours = (UINT32)(timeOfDaySeconds / UINT64(3600u));
		dt.Minutes = (UINT32)((timeOfDaySeconds / UINT64(60u)) % UINT64(60u));
		dt.Seconds = (UINT32)(timeOfDaySeconds % UINT64(60u));

		// Sub-second precision
		dt.Milliseconds = subSecondNanoseconds / UINT64(1000000u);
		dt.Microseconds = (subSecondNanoseconds / UINT64(1000u)) % UINT64(1000u);
		dt.Nanoseconds = subSecondNanoseconds % UINT64(1000u);
	}
};
