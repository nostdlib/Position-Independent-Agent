#pragma once

#include "core/core.h"

// Unified TLS buffer for both reading and writing
class TlsBuffer
{
private:
	PCHAR buffer;
	INT32 capacity;
	INT32 size;
	INT32 readPos;
	BOOL ownsMemory;
	// Dead prefix left behind by Consume(). Compaction is deferred: the live
	// region is [buffer + startPos, buffer + size) and the dead prefix is only
	// reclaimed once it exceeds capacity / 2, so a per-record Consume() is O(1)
	// instead of an O(n) memmove. Always 0 for write-mode buffers, which never
	// call Consume().
	INT32 startPos;

public:
	// Stack-only — placement new required by Result
	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}


	TlsBuffer() : buffer(nullptr), capacity(0), size(0), readPos(0), ownsMemory(true), startPos(0) {}

	// Constructor for wrapping existing data - read mode (does not own memory)
	TlsBuffer(Span<CHAR> data) : buffer(data.Data()), capacity((INT32)data.Size()), size((INT32)data.Size()), readPos(0), ownsMemory(false), startPos(0) {}

	~TlsBuffer()
	{
		if (ownsMemory)
			Clear();
	}

	TlsBuffer(const TlsBuffer &) = delete;
	TlsBuffer &operator=(const TlsBuffer &) = delete;

	TlsBuffer(TlsBuffer &&other) noexcept
		: buffer(other.buffer), capacity(other.capacity), size(other.size), readPos(other.readPos), ownsMemory(other.ownsMemory), startPos(other.startPos)
	{
		other.buffer = nullptr;
		other.capacity = 0;
		other.size = 0;
		other.readPos = 0;
		other.ownsMemory = false;
		other.startPos = 0;
	}
	TlsBuffer &operator=(TlsBuffer &&other) noexcept
	{
		if (this != &other)
		{
			if (ownsMemory)
				Clear();
			buffer = other.buffer;
			capacity = other.capacity;
			size = other.size;
			readPos = other.readPos;
			ownsMemory = other.ownsMemory;
			startPos = other.startPos;
			other.buffer = nullptr;
			other.capacity = 0;
			other.size = 0;
			other.readPos = 0;
			other.ownsMemory = false;
			other.startPos = 0;
		}
		return *this;
	}

	// Write operations
	// Append offsets are relative to GetBuffer() (the first live byte), so
	// `GetBuffer() + returnedIndex` is correct whether or not a dead prefix exists.
	INT32 Append(Span<const CHAR> data);

	template <typename T>
	INT32 Append(T value)
	{
		auto r = CheckSize(sizeof(T));
		if (!r)
			return -1;
		Memory::Copy(buffer + size, &value, sizeof(T));
		size += sizeof(T);
		return size - sizeof(T) - startPos;
	}

	INT32 AppendSize(INT32 count);
	// Setting operation
	[[nodiscard]] Result<VOID, Error> SetSize(INT32 newSize);
	VOID Clear();
	// Ensure there is enough capacity to append data
	[[nodiscard]] Result<VOID, Error> CheckSize(INT32 appendSize);

	// Read operations
	// The read cursor is an offset into the live region [GetBuffer(),
	// GetBuffer() + GetSize()), so `GetBuffer() + GetReadPosition()` is valid
	// no matter how large the dead prefix left by Consume() has grown.
	template <typename T>
	T Read()
	{
		if (readPos + (INT32)sizeof(T) > size - startPos)
		{
			readPos = size - startPos;
			return T{};
		}
		T value;
		Memory::Copy(&value, buffer + startPos + readPos, sizeof(T));
		readPos += sizeof(T);
		return value;
	}
	VOID Read(Span<CHAR> buf);
	UINT32 ReadU24BE();

	// Remove consumed bytes from the front. O(1): only advances the dead-prefix
	// cursor. The data is moved down later, by Compact(), once the dead prefix
	// exceeds capacity / 2 (amortized O(1) per byte).
	VOID Consume(INT32 bytes);

	// Reclaim the dead prefix left by Consume() by moving the live suffix down
	VOID Compact();

	// Accessors
	INT32 GetSize() const { return size - startPos; }
	// GetBuffer() points at the first LIVE byte, so GetBuffer()+GetSize() is the
	// append position and GetBuffer()-startPos is the raw allocation.
	PCHAR GetBuffer() const { return buffer + startPos; }
	// GetReadPosition() is relative to GetBuffer() (the live start), matching
	// GetSize(); both are live-region quantities, so GetReadPosition() ==
	// GetSize() means exhausted.
	INT32 GetReadPosition() const { return readPos; }
	VOID AdvanceReadPosition(INT32 sz) { readPos += sz; }
	VOID ResetReadPos() { readPos = 0; }
};
