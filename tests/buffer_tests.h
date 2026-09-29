#pragma once

#include "lib/runtime.h"
#include "tests.h"

class BufferTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Buffer Tests...");

		RunTest(allPassed, &TestBasicsSuite, "Basics suite");
		RunTest(allPassed, &TestGrowthSuite, "Growth suite");
		RunTest(allPassed, &TestMoveSemanticsSuite, "Move semantics suite");
		RunTest(allPassed, &TestEdgeCasesSuite, "Edge cases suite");
		RunTest(allPassed, &TestRoundTripSuite, "Round-trip suite");
		RunTest(allPassed, &TestGrowthLadderSuite, "Growth ladder suite");
		RunTest(allPassed, &TestReleaseDetachSuite, "Release detach suite");

		if (allPassed)
			LOG_INFO("All Buffer tests passed!");
		else
			LOG_ERROR("Some Buffer tests failed!");

		return allPassed;
	}

private:
	static BOOL TestBasicsSuite()
	{
		BOOL allPassed = true;

		// --- Default construction ---
		{
			Buffer<UINT8> b;
			BOOL passed = true;

			if (b.Data != nullptr)
			{
				LOG_ERROR("Default Data != nullptr");
				passed = false;
			}
			if (passed && (b.Capacity != 0 || b.Size != 0))
			{
				LOG_ERROR("Default Capacity/Size != 0");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Default construction");
			else
			{
				LOG_ERROR("  FAILED: Default construction");
				allPassed = false;
			}
		}

		// --- Init ---
		{
			Buffer<UINT8> b;
			BOOL passed = true;

			if (!b.Init(64))
			{
				LOG_ERROR("Init(64) returned false");
				passed = false;
			}
			if (passed && b.Data == nullptr)
			{
				LOG_ERROR("Data == nullptr after Init");
				passed = false;
			}
			if (passed && b.Capacity != 64)
			{
				LOG_ERROR("Capacity != 64 after Init");
				passed = false;
			}
			if (passed && b.Size != 0)
			{
				LOG_ERROR("Size != 0 after Init");
				passed = false;
			}
			// Re-init of an initialized buffer must fail (no leak of the old array)
			if (passed && b.Init(32))
			{
				LOG_ERROR("Init() on initialized buffer returned true");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Init allocates and stays empty");
			else
			{
				LOG_ERROR("  FAILED: Init allocates and stays empty");
				allPassed = false;
			}
		}

		// --- Add single ---
		{
			Buffer<UINT8> b;
			if (!b.Init(4)) return false;
			BOOL passed = true;

			if (!b.Add(42))
			{
				LOG_ERROR("Add(42) returned false");
				passed = false;
			}
			if (passed && b.Size != 1)
			{
				LOG_ERROR("Size != 1 after Add");
				passed = false;
			}
			if (passed && b.Data[0] != 42)
			{
				LOG_ERROR("Data[0] != 42");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Add single element");
			else
			{
				LOG_ERROR("  FAILED: Add single element");
				allPassed = false;
			}
		}

		// --- Append span ---
		{
			Buffer<CHAR> b;
			if (!b.Init(8)) return false;
			BOOL passed = true;

			const CHAR *text = "hello";
			if (!b.Append(Span<const CHAR>(text, 5)))
			{
				LOG_ERROR("Append returned false");
				passed = false;
			}
			if (passed && b.Size != 5)
			{
				LOG_ERROR("Size != 5 after Append");
				passed = false;
			}
			if (passed && (b.Data[0] != 'h' || b.Data[4] != 'o'))
			{
				LOG_ERROR("Appended bytes incorrect");
				passed = false;
			}
			// AsSpan must be bounded by Size, not Capacity
			if (passed && b.AsSpan().Size() != 5)
			{
				LOG_ERROR("AsSpan().Size() != Size");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Append span and AsSpan bounds");
			else
			{
				LOG_ERROR("  FAILED: Append span and AsSpan bounds");
				allPassed = false;
			}
		}

		// --- Unused tail ---
		{
			Buffer<UINT8> b;
			if (!b.Init(16)) return false;
			if (!b.Append(Span<const UINT8>((const UINT8 *)"1234", 4))) return false;
			BOOL passed = true;

			if (b.Unused().Size() != 12)
			{
				LOG_ERROR("Unused().Size() = %u, expected 12", (UINT32)b.Unused().Size());
				passed = false;
			}
			if (passed && b.Unused().Data() != b.Data + 4)
			{
				LOG_ERROR("Unused().Data() != Data + Size");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Unused tail region");
			else
			{
				LOG_ERROR("  FAILED: Unused tail region");
				allPassed = false;
			}
		}

		return allPassed;
	}

	static BOOL TestGrowthSuite()
	{
		BOOL allPassed = true;

		// --- Reserve growth policy ---
		{
			Buffer<UINT8> b;
			BOOL passed = true;

			// Growth from empty uses the default initial capacity
			if (!b.Reserve(1))
			{
				LOG_ERROR("Reserve(1) returned false");
				passed = false;
			}
			if (passed && b.Capacity != BufferInitialCapacity)
			{
				LOG_ERROR("Capacity = %u, expected %u", (UINT32)b.Capacity, (UINT32)BufferInitialCapacity);
				passed = false;
			}
			if (passed && b.Size != 0)
			{
				LOG_ERROR("Size != 0 after Reserve");
				passed = false;
			}

			// Doubling until the request fits
			USIZE initialCap = b.Capacity;
			if (passed && !b.Reserve(initialCap * 4))
			{
				LOG_ERROR("Reserve(%u) returned false", (UINT32)(initialCap * 4));
				passed = false;
			}
			if (passed && b.Capacity < initialCap * 4)
			{
				LOG_ERROR("Capacity = %u, expected >= %u", (UINT32)b.Capacity, (UINT32)(initialCap * 4));
				passed = false;
			}

			// Reserve within capacity is a no-op
			USIZE grownCap = b.Capacity;
			if (passed && !b.Reserve(grownCap - 1))
			{
				LOG_ERROR("Reserve(within capacity) returned false");
				passed = false;
			}
			if (passed && b.Capacity != grownCap)
			{
				LOG_ERROR("Capacity changed on no-op Reserve");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Reserve growth policy");
			else
			{
				LOG_ERROR("  FAILED: Reserve growth policy");
				allPassed = false;
			}
		}

		// --- Add beyond capacity doubles ---
		{
			Buffer<UINT32> b;
			if (!b.Init(8)) return false;
			BOOL passed = true;

			// Filling exactly to capacity does not grow (Reserve only grows when
			// the request exceeds Capacity, unlike Vector's eager off-by-one).
			USIZE initialCap = b.Capacity;
			for (USIZE i = 0; i < initialCap; i++)
			{
				if (!b.Add((UINT32)(i * 7)))
				{
					LOG_ERROR("Add failed at index %u", (UINT32)i);
					passed = false;
					break;
				}
			}
			if (passed && b.Capacity != initialCap)
			{
				LOG_ERROR("Capacity = %u, expected %u (no growth at exactly full)", (UINT32)b.Capacity, (UINT32)initialCap);
				passed = false;
			}

			// One more element past capacity doubles the allocation
			if (passed && !b.Add((UINT32)(initialCap * 7)))
			{
				LOG_ERROR("Add past capacity returned false");
				passed = false;
			}
			if (passed && b.Capacity != initialCap * 2)
			{
				LOG_ERROR("Capacity = %u, expected %u (double)", (UINT32)b.Capacity, (UINT32)(initialCap * 2));
				passed = false;
			}
			if (passed && b.Size != initialCap + 1)
			{
				LOG_ERROR("Size = %u, expected %u", (UINT32)b.Size, (UINT32)(initialCap + 1));
				passed = false;
			}
			if (passed && (b.Data[0] != 0 || b.Data[initialCap] != (UINT32)(initialCap * 7)))
			{
				LOG_ERROR("Data values incorrect after growth");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Add beyond capacity triggers growth");
			else
			{
				LOG_ERROR("  FAILED: Add beyond capacity triggers growth");
				allPassed = false;
			}
		}

		// --- Growth preserves data ---
		{
			Buffer<UINT8> b;
			BOOL passed = true;

			// Append enough chunks to trigger several doublings, verifying contents each time
			UINT8 chunk[10];
			for (UINT8 i = 0; i < 10; i++)
				chunk[i] = (UINT8)(i + 1);

			USIZE total = 0;
			for (INT32 round = 0; round < 12; round++)
			{
				if (!b.Append(Span<const UINT8>(chunk, 10)))
				{
					LOG_ERROR("Append failed on round %d", round);
					passed = false;
					break;
				}
				total += 10;
			}
			if (passed && b.Size != total)
			{
				LOG_ERROR("Size = %u, expected %u", (UINT32)b.Size, (UINT32)total);
				passed = false;
			}
			if (passed)
			{
				for (USIZE i = 0; i < total; i++)
				{
					UINT8 expected = (UINT8)((i % 10) + 1);
					if (b.Data[i] != expected)
					{
						LOG_ERROR("Data[%u] = %u, expected %u", (UINT32)i, b.Data[i], expected);
						passed = false;
						break;
					}
				}
			}

			if (passed)
				LOG_INFO("  PASSED: Growth preserves existing data");
			else
			{
				LOG_ERROR("  FAILED: Growth preserves existing data");
				allPassed = false;
			}
		}

		// --- Resize ---
		{
			Buffer<UINT8> b;
			if (!b.Init(4)) return false;
			BOOL passed = true;

			// Grow via Resize
			if (!b.Resize(64))
			{
				LOG_ERROR("Resize(64) returned false");
				passed = false;
			}
			if (passed && b.Size != 64)
			{
				LOG_ERROR("Size != 64 after Resize");
				passed = false;
			}
			// Shrink is logical only — allocation is kept
			USIZE capBefore = b.Capacity;
			if (passed && !b.Resize(4))
			{
				LOG_ERROR("Resize(4) returned false");
				passed = false;
			}
			if (passed && b.Size != 4)
			{
				LOG_ERROR("Size != 4 after shrink Resize");
				passed = false;
			}
			if (passed && b.Capacity != capBefore)
			{
				LOG_ERROR("Capacity changed on shrink Resize");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Resize grows and never shrinks allocation");
			else
			{
				LOG_ERROR("  FAILED: Resize grows and never shrinks allocation");
				allPassed = false;
			}
		}

		return allPassed;
	}

	static BOOL TestMoveSemanticsSuite()
	{
		BOOL allPassed = true;

		// --- Move construct ---
		{
			Buffer<INT32> b;
			if (!b.Init(8)) return false;
			if (!b.Add(1)) return false;
			if (!b.Add(2)) return false;

			INT32 *origData = b.Data;
			USIZE origSize = b.Size;
			USIZE origCap = b.Capacity;

			Buffer<INT32> b2((Buffer<INT32> &&)b);

			BOOL passed = true;

			if (b.Data != nullptr || b.Capacity != 0 || b.Size != 0)
			{
				LOG_ERROR("Source not zeroed after move construct");
				passed = false;
			}
			if (passed && (b2.Data != origData || b2.Size != origSize || b2.Capacity != origCap))
			{
				LOG_ERROR("Destination does not match original after move construct");
				passed = false;
			}
			if (passed && (b2.Data[0] != 1 || b2.Data[1] != 2))
			{
				LOG_ERROR("Data values incorrect after move construct");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Move construction transfers ownership");
			else
			{
				LOG_ERROR("  FAILED: Move construction transfers ownership");
				allPassed = false;
			}
		}

		// --- Move assign ---
		{
			Buffer<INT32> b;
			if (!b.Init(8)) return false;
			if (!b.Add(10)) return false;
			if (!b.Add(20)) return false;

			INT32 *origData = b.Data;

			Buffer<INT32> b2;
			b2 = (Buffer<INT32> &&)b;

			BOOL passed = true;

			if (b.Data != nullptr || b.Capacity != 0 || b.Size != 0)
			{
				LOG_ERROR("Source not zeroed after move assign");
				passed = false;
			}
			if (passed && (b2.Data != origData || b2.Size != 2))
			{
				LOG_ERROR("Destination incorrect after move assign");
				passed = false;
			}
			if (passed && (b2.Data[0] != 10 || b2.Data[1] != 20))
			{
				LOG_ERROR("Data values incorrect after move assign");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Move assignment transfers ownership");
			else
			{
				LOG_ERROR("  FAILED: Move assignment transfers ownership");
				allPassed = false;
			}
		}

		// --- Self-assign ---
		{
			Buffer<INT32> b;
			if (!b.Init(8)) return false;
			if (!b.Add(99)) return false;

			INT32 *origData = b.Data;
			b = (Buffer<INT32> &&)b;

			BOOL passed = b.Data == origData && b.Size == 1 && b.Data[0] == 99;

			if (passed)
				LOG_INFO("  PASSED: Move self-assignment is safe");
			else
			{
				LOG_ERROR("Self move-assign corrupted data");
				LOG_ERROR("  FAILED: Move self-assignment is safe");
				allPassed = false;
			}
		}

		// --- Move assign into non-empty ---
		{
			Buffer<INT32> b1;
			if (!b1.Init(4)) return false;
			if (!b1.Add(100)) return false;

			Buffer<INT32> b2;
			if (!b2.Init(4)) return false;
			if (!b2.Add(200)) return false;
			if (!b2.Add(300)) return false;

			b2 = (Buffer<INT32> &&)b1;

			BOOL passed = true;

			if (b2.Size != 1 || b2.Data[0] != 100)
			{
				LOG_ERROR("b2 should have b1's data after move");
				passed = false;
			}
			if (passed && (b1.Data != nullptr || b1.Size != 0))
			{
				LOG_ERROR("b1 not zeroed after move");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Move assign into non-empty buffer");
			else
			{
				LOG_ERROR("  FAILED: Move assign into non-empty buffer");
				allPassed = false;
			}
		}

		return allPassed;
	}

	static BOOL TestEdgeCasesSuite()
	{
		BOOL allPassed = true;

		// --- Release ---
		{
			Buffer<UINT8> b;
			if (!b.Init(8)) return false;
			if (!b.Add(7)) return false;
			if (!b.Add(8)) return false;

			UINT8 *released = b.Release();
			BOOL passed = true;

			if (released == nullptr)
			{
				LOG_ERROR("Release() returned nullptr");
				passed = false;
			}
			if (passed && (released[0] != 7 || released[1] != 8))
			{
				LOG_ERROR("Released data values incorrect");
				passed = false;
			}
			if (passed && (b.Data != nullptr || b.Capacity != 0 || b.Size != 0))
			{
				LOG_ERROR("Buffer not reset after Release()");
				passed = false;
			}

			if (released)
				delete[] released;

			if (passed)
				LOG_INFO("  PASSED: Release returns pointer and resets");
			else
			{
				LOG_ERROR("  FAILED: Release returns pointer and resets");
				allPassed = false;
			}
		}

		// --- Release of an empty buffer ---
		{
			Buffer<UINT8> b;
			BOOL passed = b.Release() == nullptr && b.Data == nullptr && b.Size == 0;

			if (passed)
				LOG_INFO("  PASSED: Release on empty buffer returns nullptr");
			else
			{
				LOG_ERROR("  FAILED: Release on empty buffer returns nullptr");
				allPassed = false;
			}
		}

		// --- Reset then reuse ---
		{
			Buffer<UINT8> b;
			if (!b.Init(8)) return false;
			for (UINT8 i = 0; i < 8; i++)
				if (!b.Add(i)) return false;

			USIZE capBefore = b.Capacity;
			b.Reset();

			BOOL passed = true;
			if (b.Size != 0)
			{
				LOG_ERROR("Size != 0 after Reset");
				passed = false;
			}
			if (passed && b.Capacity != capBefore)
			{
				LOG_ERROR("Capacity not retained after Reset");
				passed = false;
			}
			// Reuse: appending within the retained capacity must not reallocate
			UINT8 *dataBefore = b.Data;
			if (passed && !b.Append(Span<const UINT8>((const UINT8 *)"abc", 3)))
			{
				LOG_ERROR("Append after Reset returned false");
				passed = false;
			}
			if (passed && b.Data != dataBefore)
			{
				LOG_ERROR("Backing array changed on reuse within retained capacity");
				passed = false;
			}
			if (passed && (b.Data[0] != 'a' || b.Data[2] != 'c' || b.Size != 3))
			{
				LOG_ERROR("Reused region contents incorrect");
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Reset keeps capacity for reuse");
			else
			{
				LOG_ERROR("  FAILED: Reset keeps capacity for reuse");
				allPassed = false;
			}
		}

		// --- Zero-length Append ---
		{
			Buffer<UINT8> b;
			if (!b.Init(4)) return false;
			BOOL passed = b.Append(Span<const UINT8>()) && b.Size == 0;

			if (passed)
				LOG_INFO("  PASSED: Zero-length Append is a no-op");
			else
			{
				LOG_ERROR("  FAILED: Zero-length Append is a no-op");
				allPassed = false;
			}
		}

		return allPassed;
	}

	// Append → Resize → Reset → Append → Release chained on one buffer
	static BOOL TestRoundTripSuite()
	{
		BOOL allPassed = true;

		// --- Append/Resize/Reset/Release round-trip ---
		{
			Buffer<UINT8> b;
			BOOL passed = true;

			if (!b.Init(8))
				return false;
			if (!b.Append(Span<const UINT8>((const UINT8 *)"abcdefgh", 8)))
				passed = false;

			// Grow via Resize: the existing prefix must survive
			if (passed && !b.Resize(64))
				passed = false;
			if (passed && (b.Size != 64 || b.Capacity < 64))
				passed = false;
			if (passed && Memory::Compare(b.Data, "abcdefgh", 8) != 0)
				passed = false;

			// Shrink is logical only, then Reset keeps the allocation
			if (passed && !b.Resize(8))
				passed = false;
			b.Reset();
			if (passed && (b.Size != 0 || b.Capacity < 64))
				passed = false;

			// Append after Reset lands at the front of the retained array
			UINT8 *dataBefore = b.Data;
			if (passed && !b.Append(Span<const UINT8>((const UINT8 *)"XY", 2)))
				passed = false;
			if (passed && (b.Size != 2 || b.Data != dataBefore || b.Data[0] != 'X' || b.Data[1] != 'Y'))
				passed = false;

			// Release detaches; the buffer is reusable afterwards
			UINT8 *detached = b.Release();
			if (passed && (detached != dataBefore || b.Data != nullptr || b.Capacity != 0 || b.Size != 0))
				passed = false;
			if (passed && !b.Init(4))
				passed = false;
			if (passed && (!b.Add(7) || b.Size != 1 || b.Data[0] != 7))
				passed = false;

			delete[] detached;

			if (passed)
				LOG_INFO("  PASSED: Append/Resize/Reset/Release round-trip");
			else
			{
				LOG_ERROR("  FAILED: Append/Resize/Reset/Release round-trip");
				allPassed = false;
			}
		}

		// --- Resize growth uses the doubling ladder ---
		{
			Buffer<UINT8> b;
			if (!b.Init(4)) return false;
			for (UINT8 i = 0; i < 4; i++)
				if (!b.Add(i)) return false;

			BOOL passed = true;
			// Size 4 → Resize(100): 4→8→…→128 is the first doubling ≥ 100
			if (!b.Resize(100))
				passed = false;
			if (passed && b.Capacity != 128)
			{
				LOG_ERROR("Capacity = %u after Resize(100), expected 128", (UINT32)b.Capacity);
				passed = false;
			}
			if (passed && b.Size != 100)
				passed = false;
			// The four appended elements are still at the front
			for (UINT8 i = 0; passed && i < 4; i++)
				if (b.Data[i] != i)
					passed = false;

			if (passed)
				LOG_INFO("  PASSED: Resize growth uses the doubling ladder");
			else
			{
				LOG_ERROR("  FAILED: Resize growth uses the doubling ladder");
				allPassed = false;
			}
		}

		// --- Append after a shrink Resize keeps the tail intact ---
		{
			Buffer<UINT16> b;
			if (!b.Init(4)) return false;
			for (UINT16 i = 1; i <= 4; i++)
				if (!b.Add(i * 0x100)) return false;

			BOOL passed = true;
			if (!b.Resize(2) || !b.Add(0xBEEF))
				passed = false;
			// The append must land exactly at index 2, not at the old Size 4
			if (passed && (b.Size != 3 || b.Data[2] != 0xBEEF || b.Data[0] != 0x100 || b.Data[1] != 0x200))
				passed = false;

			if (passed)
				LOG_INFO("  PASSED: Append after shrink Resize lands at Size");
			else
			{
				LOG_ERROR("  FAILED: Append after shrink Resize lands at Size");
				allPassed = false;
			}
		}

		return allPassed;
	}

	// Exact capacity boundaries of the doubling growth policy
	static BOOL TestGrowthLadderSuite()
	{
		BOOL allPassed = true;

		// --- Fill to exactly capacity: no growth; one more: double ---
		{
			Buffer<UINT32> b;
			BOOL passed = true;

			USIZE expected = BufferInitialCapacity;
			for (INT32 round = 0; round < 4 && passed; round++)
			{
				while (b.Size < expected)
				{
					if (!b.Add((UINT32)b.Size))
					{
						passed = false;
						break;
					}
				}
				if (passed && b.Capacity != expected)
				{
					LOG_ERROR("Capacity = %u at Size %u, expected %u",
					          (UINT32)b.Capacity, (UINT32)b.Size, (UINT32)expected);
					passed = false;
				}
				if (passed)
				{
					// Exactly full must NOT reallocate (Reserve only grows past Capacity)
					UINT32 *dataBefore = b.Data;
					if (!b.Add(0xABCDEF))
						passed = false;
					else if (b.Data == dataBefore || b.Capacity != expected * 2)
					{
						LOG_ERROR("Capacity = %u past full, expected %u with a new array",
						          (UINT32)b.Capacity, (UINT32)(expected * 2));
						passed = false;
					}
				}
				expected *= 2;
			}

			if (passed)
				LOG_INFO("  PASSED: Doubling ladder 16/32/64/128 with exact-fill boundaries");
			else
			{
				LOG_ERROR("  FAILED: Doubling ladder 16/32/64/128 with exact-fill boundaries");
				allPassed = false;
			}
		}

		// --- One large Append jumps straight to the first power-of-two fit ---
		{
			Buffer<UINT8> b;
			BOOL passed = b.Append(Span<const UINT8>((const UINT8 *)"x", 1)) && b.Capacity == BufferInitialCapacity;

			UINT8 chunk[1000];
			Memory::Set(chunk, 0x5A, sizeof(chunk));
			if (passed && !b.Append(Span<const UINT8>(chunk, sizeof(chunk))))
				passed = false;
			if (passed && b.Capacity != 1024)
			{
				LOG_ERROR("Capacity = %u after Append(1000), expected 1024", (UINT32)b.Capacity);
				passed = false;
			}
			if (passed && (b.Size != 1001 || b.Data[0] != 'x' || b.Data[1] != 0x5A || b.Data[1000] != 0x5A))
				passed = false;

			if (passed)
				LOG_INFO("  PASSED: Single large Append doubles until it fits");
			else
			{
				LOG_ERROR("  FAILED: Single large Append doubles until it fits");
				allPassed = false;
			}
		}

		// --- Growth from a non-power-of-two capacity lands exactly on the request ---
		{
			Buffer<UINT8> b;
			if (!b.Init(5)) return false;
			BOOL passed = true;

			UINT8 chunk[40];
			Memory::Set(chunk, 0xC3, sizeof(chunk));
			if (!b.Append(Span<const UINT8>(chunk, sizeof(chunk))))
				passed = false;
			// 5→10→20→40: the ladder stops exactly at the request
			if (passed && b.Capacity != 40)
			{
				LOG_ERROR("Capacity = %u, expected 40", (UINT32)b.Capacity);
				passed = false;
			}

			if (passed)
				LOG_INFO("  PASSED: Non-power-of-two start lands exactly on the request");
			else
			{
				LOG_ERROR("  FAILED: Non-power-of-two start lands exactly on the request");
				allPassed = false;
			}
		}

		return allPassed;
	}

	// Release() must fully detach the array, not just drop it
	static BOOL TestReleaseDetachSuite()
	{
		BOOL allPassed = true;

		// --- Pointer identity and independence after Release ---
		{
			Buffer<UINT64> b;
			if (!b.Init(8)) return false;
			for (UINT64 i = 0; i < 6; i++)
				if (!b.Add(i + 1)) return false;

			UINT64 *origData = b.Data;
			UINT64 *released = b.Release();

			BOOL passed = true;
			if (released != origData)
			{
				LOG_ERROR("Release() returned a different pointer than Data");
				passed = false;
			}
			// The detached array keeps its contents and is independently usable
			if (passed && (released[0] != 1 || released[5] != 6))
				passed = false;
			released[0] = 0xDEADBEEF;
			if (passed && (b.Data != nullptr || b.Capacity != 0 || b.Size != 0))
			{
				LOG_ERROR("Buffer not fully reset by Release()");
				passed = false;
			}

			// New writes through the buffer must not touch the detached array
			if (passed && b.Init(4) && b.Add(99) && released[0] != 0xDEADBEEF)
				passed = false;

			delete[] released;

			if (passed)
				LOG_INFO("  PASSED: Release detaches the exact array and stays independent");
			else
			{
				LOG_ERROR("  FAILED: Release detaches the exact array and stays independent");
				allPassed = false;
			}
		}

		// --- Release after Reset still returns the allocation ---
		{
			Buffer<UINT8> b;
			if (!b.Init(8)) return false;
			if (!b.Add(1)) return false;
			UINT8 *dataBefore = b.Data;

			b.Reset();
			UINT8 *released = b.Release();

			BOOL passed = released == dataBefore && b.Data == nullptr && b.Capacity == 0 && b.Size == 0;
			delete[] released;

			if (passed)
				LOG_INFO("  PASSED: Release after Reset returns the allocation");
			else
			{
				LOG_ERROR("  FAILED: Release after Reset returns the allocation");
				allPassed = false;
			}
		}

		return allPassed;
	}
};
