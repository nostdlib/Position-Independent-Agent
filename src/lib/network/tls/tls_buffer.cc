#include "lib/network/tls/tls_buffer.h"
#include "core/memory/memory.h"
#include "platform/console/logger.h"

INT32 TlsBuffer::Append(Span<const CHAR> data)
{
	auto r = CheckSize((INT32)data.Size());
	if (!r)
		return -1;
	Memory::Copy(buffer + size, data.Data(), data.Size());
	size += (INT32)data.Size();
	return size - (INT32)data.Size() - startPos;
}

INT32 TlsBuffer::AppendSize(INT32 count)
{
	auto r = CheckSize(count);
	if (!r)
		return -1;
	size += count;
	return size - count - startPos;
}

Result<VOID, Error> TlsBuffer::SetSize(INT32 newSize)
{
	// SetSize is a write-mode operation: it discards any dead prefix too
	startPos = 0;
	readPos = 0;
	size = 0;
	auto r = CheckSize(newSize);
	if (!r)
		return Result<VOID, Error>::Err(r.Error());
	size = newSize;
	return Result<VOID, Error>::Ok();
}

VOID TlsBuffer::Clear()
{
	if (buffer && ownsMemory)
	{
		delete[] buffer;
	}
	buffer = nullptr;
	size = 0;
	capacity = 0;
	readPos = 0;
	startPos = 0;
}

Result<VOID, Error> TlsBuffer::CheckSize(INT32 appendSize)
{
	if (size + appendSize <= capacity)
	{
		LOG_DEBUG("Buffer size is sufficient: %d + %d <= %d", size, appendSize, capacity);
		return Result<VOID, Error>::Ok();
	}

	// A wrapped (non-owning) buffer cannot be grown
	if (!ownsMemory)
		return Result<VOID, Error>::Err(Error::TlsBuffer_AllocationFailed);

	// The dead prefix may be all that blocks the fit: reclaim it before
	// deciding to grow, mirroring ByteQueue::CheckSize. Compact() moves the
	// live bytes within the same allocation; pointers into this buffer must
	// not be held across this call either way — growth would reallocate them.
	if ((size - startPos) + appendSize <= capacity)
	{
		Compact();
		return Result<VOID, Error>::Ok();
	}

	// Delegate the grow-and-copy to the core container: the new capacity keeps
	// TlsBuffer's original sizing (4x the request, floored at 256 bytes).
	// Only the live bytes are carried over, and the sizing uses the
	// post-compaction footprint so the dead prefix does not inflate the
	// new allocation.
	UINT32 liveNeed = (UINT32)(size - startPos) + (UINT32)appendSize;
	UINT32 newLen = liveNeed * 4;
	if (newLen < 256)
		newLen = 256;
	if (newLen < liveNeed)
		newLen = liveNeed; // *4 overflow guard

	Buffer<CHAR> grown;
	if (!grown.Init(newLen) || !grown.Append(Span<const CHAR>(buffer + startPos, (USIZE)(size - startPos))))
		return Result<VOID, Error>::Err(Error::TlsBuffer_AllocationFailed);

	LOG_DEBUG("Resizing buffer from %d to %u bytes", capacity, newLen);

	delete[] buffer;
	buffer = grown.Release();
	capacity = (INT32)newLen;
	// The dead prefix is gone: size shrinks to the live byte count. readPos
	// needs no adjustment — it is live-relative, exactly like Compact().
	size -= startPos;
	startPos = 0;
	ownsMemory = true;
	return Result<VOID, Error>::Ok();
}

/// @brief Remove consumed bytes from the front without moving data
/// @param bytes Number of bytes to consume from the front of the buffer
/// @return void
/// @note O(1) — advances the dead-prefix cursor. The live suffix is moved down
///       later, by Compact(), once the dead prefix exceeds capacity / 2.
VOID TlsBuffer::Consume(INT32 bytes)
{
	if (bytes <= 0)
		return;
	// Compare against the live size, not startPos + bytes: with startPos > 0
	// the sum can overflow INT32 for a large byte count (signed overflow is UB,
	// and a wrapped value would skip the reset below), while size - startPos
	// cannot overflow because startPos <= size always holds.
	if (bytes >= size - startPos)
	{
		// Everything consumed: reset to an empty buffer, keep the allocation
		size = 0;
		startPos = 0;
		readPos = 0;
		return;
	}
	startPos += bytes;
	// The consumed bytes are done with, so the read cursor restarts at the
	// new live start (readPos is live-relative)
	readPos = 0;
	if (startPos > (capacity >> 1))
		Compact();
}

VOID TlsBuffer::Compact()
{
	if (startPos == 0)
		return;
	INT32 dead = startPos;
	INT32 live = size - dead;
	if (live > 0)
		Memory::Move(buffer, buffer + dead, live);
	size = live;
	startPos = 0;
	// readPos needs no adjustment: it is live-relative, so it still addresses
	// the same logical byte after the move
}


VOID TlsBuffer::Read(Span<CHAR> buf)
{
	INT32 available = (size - startPos) - readPos;
	INT32 count = (INT32)buf.Size();
	
	if (count > available)
		count = available;
	if (count > 0)
		Memory::Copy(buf.Data(), buffer + startPos + readPos, count);
	readPos += count;
}

/// @brief Read a 24-bit big-endian unsigned integer from the TLS buffer
/// @return The 24-bit value read from the buffer
UINT32 TlsBuffer::ReadU24BE()
{
	// Ensure there are at least 3 bytes available to read (24 bits)
	if (readPos + 3 > size - startPos)
	{
		readPos = size - startPos;
		return 0;
	}
	PCHAR p = buffer + startPos + readPos;
	UINT8 b0 = (UINT8)p[0];
	UINT8 b1 = (UINT8)p[1];
	UINT8 b2 = (UINT8)p[2];
	readPos += 3;
	return ((UINT32)b0 << 16) | ((UINT32)b1 << 8) | (UINT32)b2;
}
