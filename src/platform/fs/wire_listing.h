/**
 * @file wire_listing.h
 * @brief Compact (format v3) directory-listing wire encoder
 *
 * @details Encodes a DirectoryEntry array into the variable-length listing
 * frame the C2 parses: a 12-byte header [status:u32 = 0][entryCount:u32]
 * [formatVersion:u32 = 3] followed by per-entry
 * [nameLen:u16][name: WTF-8][attrs:u8][size: LEB128 u64]
 * [creationTime:u32 unix-seconds][lastModifiedTime:u32 unix-seconds]
 * (+ [volumeSerial:u32] for drive entries only) — ~26 bytes for a typical
 * entry against the legacy fixed 553-byte block. Names are WTF-8
 * (UTF16::ToWTF8): unpaired surrogates ride as 3-byte sequences, so
 * arbitrary NTFS UTF-16 names and POSIX surrogateescape names round-trip
 * byte-exactly; the C2's strict decoder reverses it.
 *
 * Lives in the platform layer (not src/beacon) because the test binary
 * swaps the app layer out under BUILD_TESTS — here the encoder compiles
 * into both and its golden vectors run on every host.
 *
 * @see C2 Features/Operations/FileSystem/DirEntry.cs (ParseV3) — the
 * parsing half; the two must stay byte-compatible.
 */

#pragma once

#include "core/containers/buffer.h"
#include "core/encoding/utf16.h"
#include "core/memory/memory.h"
#include "core/string/string.h"
#include "platform/fs/directory_entry.h"

/// In-band listing format generation. Rides the frame header (offset 8), NOT
/// X-Api-Version — the C2 registration gate accepts only versions 0/1, and it
/// dispatches on this word instead: 3 = this layout, 0 = legacy fixed stride.
constexpr UINT32 LISTING_FORMAT_VERSION = 3;

/// What DirectoryEntry::CreationTime/LastModifiedTime hold on the building
/// platform — passed in by the caller (the beacon layer knows its platform)
/// so the encoder itself stays platform-neutral and BOTH conversion paths are
/// testable on any host.
enum class ListingTimeEncoding : UINT8
{
	ListingTime_FileTime = 0,    ///< Windows/WPD: raw FILETIME (100ns ticks since 1601-01-01)
	ListingTime_UnixSeconds = 1, ///< POSIX st_mtime seconds (UEFI sends zeros)
};

/// @brief Static v3 compact listing encoder
/// @details Two passes over the entries: an exact-size pass (every byte
/// accounted before any allocation — a 50k-entry listing is one allocation,
/// no growth copies), then an in-place write pass. All writes are
/// bounds-checked against the reserved total, so a drift between the passes
/// is a clean error, never an overflow.
class WireListing
{
public:
	/// [status:u32][entryCount:u32][formatVersion:u32]
	static constexpr USIZE HeaderSize = 12;
	/// Name[256] minus the terminator slot every platform backend respects
	static constexpr USIZE MaxNameUnits = 255;
	/// Staging bound: units × 4 — covers 16-bit WCHAR (astral pairs, 2 B/unit
	/// → ≤ 765) and 32-bit WCHAR (astral single units, 4 B/unit). Real filename
	/// byte budgets keep every platform under this; the exact-size pass drift
	/// check turns a violation into a clean error, never an overflow.
	static constexpr USIZE MaxNameBytes = MaxNameUnits * 4;

	/**
	 * @brief Encode one listing into an exact-size buffer
	 * @param entries The platform-layer entries to serialize
	 * @param enc What the entry timestamps hold on this platform
	 * @param out Receives the released-ready frame (Size = total, one allocation)
	 * @return Ok on success; Err(Buffer_AllocationFailed) when the exact-size
	 *         allocation fails, Err(Buffer_InvalidState) on size overflow or
	 *         pass drift
	 * @note The status word is written as the literal 0 (success) — StatusCode
	 *       lives in the beacon layer, which this platform-layer header must
	 *       not include; 0 is its wire value.
	 */
	[[nodiscard]] static Result<VOID, Error> Encode(Span<const DirectoryEntry> entries,
													ListingTimeEncoding enc, Buffer<CHAR> &out)
	{
		USIZE total = HeaderSize;
		for (USIZE i = 0; i < entries.Size(); i++)
		{
			Result<VOID, Error> sized = AccumulateEntrySize(entries[i], total);
			if (!sized)
				return sized;
		}

		Result<VOID, Error> init = out.Init(total);
		if (!init)
			return init;

		USIZE offset = HeaderSize;
		for (USIZE i = 0; i < entries.Size(); i++)
		{
			Result<VOID, Error> written = AppendEntry(out.Data, offset, total, entries[i], enc);
			if (!written)
				return written;
		}
		if (offset != total)
			return Result<VOID, Error>::Err(Error::Buffer_InvalidState); // pass drift — refuse to ship

		StoreU32(out.Data + 0, 0); // status = success (see note above)
		StoreU32(out.Data + 4, (UINT32)entries.Size());
		StoreU32(out.Data + 8, LISTING_FORMAT_VERSION);
		return out.Resize(total);
	}

	/**
	 * @brief Unix-epoch seconds (u32 wire domain) from a raw platform timestamp
	 * @param raw CreationTime/LastModifiedTime as the platform filled it
	 * @param enc What @p raw holds
	 * @return 0 for 0 (unknown stays unknown) and pre-1970 FILETIMEs;
	 *         post-2106 values saturate at 0xFFFFFFFF (far future, not unknown)
	 */
	static UINT32 ToUnixSeconds(UINT64 raw, ListingTimeEncoding enc)
	{
		if (raw == 0)
			return 0;
		UINT64 seconds;
		if (enc == ListingTimeEncoding::ListingTime_UnixSeconds)
		{
			// A negative st_mtime cast to UINT64 would saturate to year-2106 —
			// pre-1970 timestamps are unknown (0), not far future.
			if ((INT64)raw < 0)
				return 0;
			seconds = raw;
		}
		else
		{
			constexpr UINT64 UnixEpochFileTime = 11644473600ull * 10000000ull; // 1601→1970 in FILETIME ticks
			if (raw < UnixEpochFileTime)
				return 0;
			seconds = (raw - UnixEpochFileTime) / 10000000ull;
		}
		return seconds > 0xFFFFFFFFull ? 0xFFFFFFFFu : (UINT32)seconds;
	}

private:
	WireListing() = delete; // static-only

	/// Copy the entry's name units into an ALIGNED staging buffer, byte-assembled,
	/// and return the unit count. DirectoryEntry is pack(1), so its WCHAR array
	/// can sit at any byte offset — a wchar load from it faults on
	/// strict-alignment targets (MIPS32/ARMv7 o32), which is exactly what the
	/// qemu CI caught. The scan is also BOUNDED: a backend that fills all 256
	/// units without a terminator must not run the walk off the array.
	static USIZE StageName(const DirectoryEntry &entry, WCHAR (&staging)[MaxNameUnits])
	{
		const UINT8 *raw = (const UINT8 *)entry.Name;
		USIZE units = 0;
		while (units < MaxNameUnits)
		{
			// Assemble the unit from bytes in little-endian order (every default
			// target is LE; zero-detection is order-independent either way).
			UINT32 unit = 0;
			for (USIZE b = 0; b < sizeof(WCHAR); b++)
				unit |= (UINT32)raw[units * sizeof(WCHAR) + b] << (8 * b);
			if (unit == 0)
				break;
			staging[units++] = (WCHAR)unit;
		}
		return units;
	}

	/// Five flag bits + the drive type in bits 5-7 (masked to the 3-bit wire
	/// field; plain entries carry a placeholder the C2 ignores).
	static UINT8 EncodeAttrs(const DirectoryEntry &entry)
	{
		return (UINT8)((entry.IsDirectory ? 1 : 0)
					   | (entry.IsDrive ? 2 : 0)
					   | (entry.IsHidden ? 4 : 0)
					   | (entry.IsSystem ? 8 : 0)
					   | (entry.IsReadOnly ? 16 : 0)
					   | ((entry.Type & 7u) << 5));
	}

	/// LEB128 length of a u64 (1-10 bytes).
	static USIZE VarintLength(UINT64 value)
	{
		USIZE length = 1;
		while ((value >>= 7) != 0)
			length++;
		return length;
	}

	/// LEB128-encode a u64 into @p out (at least VarintLength(value) bytes); returns the byte count.
	static USIZE EncodeVarint(UINT64 value, CHAR *out)
	{
		USIZE count = 0;
		do
		{
			UINT8 byte = (UINT8)(value & 0x7F);
			value >>= 7;
			if (value != 0)
				byte |= 0x80;
			out[count++] = (CHAR)byte;
		} while (value != 0);
		return count;
	}

	static VOID StoreU16(CHAR *target, UINT16 value) { Memory::Copy(target, &value, sizeof(UINT16)); }
	static VOID StoreU32(CHAR *target, UINT32 value) { Memory::Copy(target, &value, sizeof(UINT32)); }

	/// On-wire size of one entry given its encoded name length — the single sizing
	/// expression both passes share, so they cannot drift apart.
	static USIZE EntryWireSize(const DirectoryEntry &entry, USIZE nameBytes)
	{
		return 2 + nameBytes + 1 + VarintLength(entry.Size) + 4 + 4 + (entry.IsDrive ? 4 : 0);
	}

	static Result<VOID, Error> AccumulateEntrySize(const DirectoryEntry &entry, USIZE &total)
	{
		WCHAR staging[MaxNameUnits];
		USIZE entrySize = EntryWireSize(entry,
			UTF16::WTF8Length(Span<const WCHAR>(staging, StageName(entry, staging))));
		if (total + entrySize < total)
			return Result<VOID, Error>::Err(Error::Buffer_InvalidState); // USIZE overflow
		total += entrySize;
		return Result<VOID, Error>::Ok();
	}

	/// Write one entry at base[offset..], bounds-checked against the pass-1
	/// total — a drift between the passes faults here instead of overflowing.
	static Result<VOID, Error> AppendEntry(CHAR *base, USIZE &offset, USIZE limit,
										   const DirectoryEntry &entry, ListingTimeEncoding enc)
	{
		WCHAR staging[MaxNameUnits];
		USIZE units = StageName(entry, staging);
		// +4 headroom: ToWTF8's loop reserves room for a maximal 4-byte tail
		CHAR name[MaxNameBytes + 4];
		USIZE nameBytes = UTF16::ToWTF8(Span<const WCHAR>(staging, units),
										Span<CHAR>(name, sizeof(name)));

		USIZE need = EntryWireSize(entry, nameBytes);
		if (offset + need < offset || offset + need > limit)
			return Result<VOID, Error>::Err(Error::Buffer_InvalidState);

		StoreU16(base + offset, (UINT16)nameBytes);
		offset += 2;
		Memory::Copy(base + offset, name, nameBytes);
		offset += nameBytes;
		base[offset++] = (CHAR)EncodeAttrs(entry);
		offset += EncodeVarint(entry.Size, base + offset);
		StoreU32(base + offset, ToUnixSeconds(entry.CreationTime, enc));
		offset += 4;
		StoreU32(base + offset, ToUnixSeconds(entry.LastModifiedTime, enc));
		offset += 4;
		if (entry.IsDrive)
		{
			// Windows volume serials are u32-valued; WPD device tokens are 64-bit
			// hashes — fold the high half in rather than truncating it away (the
			// full identity also rides in the entry name, "::mtp-<hex>").
			StoreU32(base + offset, (UINT32)(entry.VolumeSerial ^ (entry.VolumeSerial >> 32)));
			offset += 4;
		}
		return Result<VOID, Error>::Ok();
	}
};
