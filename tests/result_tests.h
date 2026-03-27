#pragma once

#include "lib/runtime.h"
#include "tests.h"

class ResultTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Result Tests...");

		RunTest(allPassed, &TestConstructionSuite, "Construction suite");
		RunTest(allPassed, &TestMoveSemanticsSuite, "Move semantics suite");
		RunTest(allPassed, &TestErrorChainingSuite, "Error chaining suite");
		RunTest(allPassed, &TestCompactSpecializationSuite, "Compact specialization suite");
		RunTest(allPassed, &TestErrorAccessorsSuite, "Error accessors suite");

		if (allPassed)
			LOG_INFO("All Result tests passed!");
		else
			LOG_ERROR("Some Result tests failed!");

		return allPassed;
	}

private:
	// Move-only RAII helper that tracks destruction via a boolean flag.
	struct Tracked
	{
		UINT32 value;
		BOOL *destroyed;

		Tracked(UINT32 v, BOOL *flag) : value(v), destroyed(flag) {}
		~Tracked()
		{
			if (destroyed)
				*destroyed = true;
		}

		Tracked(Tracked &&other) noexcept
			: value(other.value), destroyed(other.destroyed)
		{
			other.destroyed = nullptr;
		}

		Tracked &operator=(Tracked &&other) noexcept
		{
			value = other.value;
			destroyed = other.destroyed;
			other.destroyed = nullptr;
			return *this;
		}

		Tracked(const Tracked &) = delete;
		Tracked &operator=(const Tracked &) = delete;
	};

	// =====================================================================
	// Construction suite
	// =====================================================================

	static BOOL TestConstructionSuite()
	{
		BOOL allPassed = true;

		// Ok construction
		{
			auto r = Result<UINT32, UINT32>::Ok(42);
			if (!r.IsOk())
			{
				LOG_ERROR("  FAILED: Ok construction (Ok(42).IsOk() returned false)");
				allPassed = false;
			}
			else if (r.Value() != 42)
			{
				LOG_ERROR("  FAILED: Ok construction (Ok(42).Value() != 42, got %u)", r.Value());
				allPassed = false;
			}
			else
			{
				LOG_INFO("  PASSED: Ok construction");
			}
		}

		// Err construction
		{
			auto r = Result<UINT32, UINT32>::Err(99);
			if (!r.IsErr())
			{
				LOG_ERROR("  FAILED: Err construction (Err(99).IsErr() returned false)");
				allPassed = false;
			}
			else
			{
				LOG_INFO("  PASSED: Err construction");
			}
		}

		// Void Ok construction
		{
			auto r = Result<VOID, UINT32>::Ok();
			if (!r.IsOk())
			{
				LOG_ERROR("  FAILED: Void Ok construction (Ok().IsOk() returned false)");
				allPassed = false;
			}
			else if (r.IsErr())
			{
				LOG_ERROR("  FAILED: Void Ok construction (Ok().IsErr() returned true)");
				allPassed = false;
			}
			else
			{
				LOG_INFO("  PASSED: Void Ok construction");
			}
		}

		// Void Err construction
		{
			auto r = Result<VOID, Error>::Err(Error::Socket_CreateFailed_Open);
			if (!r.IsErr())
			{
				LOG_ERROR("  FAILED: Void Err construction (Err().IsErr() returned false)");
				allPassed = false;
			}
			else if (r.IsOk())
			{
				LOG_ERROR("  FAILED: Void Err construction (Err().IsOk() returned true)");
				allPassed = false;
			}
			else
			{
				const Error &err = r.Error();
				if (err.Code != Error::Socket_CreateFailed_Open)
				{
					LOG_ERROR("  FAILED: Void Err construction (Code mismatch)");
					allPassed = false;
				}
				else if (err.Platform != Error::PlatformKind::Runtime)
				{
					LOG_ERROR("  FAILED: Void Err construction (Platform mismatch)");
					allPassed = false;
				}
				else if (err.Depth != 0)
				{
					LOG_ERROR("  FAILED: Void Err construction (Depth != 0, got %u)", (UINT32)err.Depth);
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Void Err construction");
				}
			}
		}

		// IsOk/IsErr mutual exclusivity
		{
			auto ok = Result<UINT32, UINT32>::Ok(1);
			auto err = Result<UINT32, UINT32>::Err(2);

			if (!ok.IsOk() || ok.IsErr())
			{
				LOG_ERROR("  FAILED: IsOk/IsErr (Ok result not mutually exclusive)");
				allPassed = false;
			}
			else if (!err.IsErr() || err.IsOk())
			{
				LOG_ERROR("  FAILED: IsOk/IsErr (Err result not mutually exclusive)");
				allPassed = false;
			}
			else
			{
				LOG_INFO("  PASSED: IsOk/IsErr mutual exclusivity");
			}
		}

		// operator BOOL
		{
			auto ok = Result<UINT32, UINT32>::Ok(1);
			auto err = Result<UINT32, UINT32>::Err(2);

			if (!(BOOL)ok)
			{
				LOG_ERROR("  FAILED: operator BOOL ((BOOL)ok returned false)");
				allPassed = false;
			}
			else if ((BOOL)err)
			{
				LOG_ERROR("  FAILED: operator BOOL ((BOOL)err returned true)");
				allPassed = false;
			}
			else if (!ok)
			{
				LOG_ERROR("  FAILED: operator BOOL (!ok evaluated to true)");
				allPassed = false;
			}
			else if (err)
			{
				LOG_ERROR("  FAILED: operator BOOL (err evaluated to true)");
				allPassed = false;
			}
			else
			{
				LOG_INFO("  PASSED: operator BOOL");
			}
		}

		// Value access
		{
			auto r = Result<UINT32, UINT32>::Ok(123);
			if (r.Value() != 123)
			{
				LOG_ERROR("  FAILED: Value access (Value() != 123, got %u)", r.Value());
				allPassed = false;
			}
			else
			{
				const auto &cr = r;
				if (cr.Value() != 123)
				{
					LOG_ERROR("  FAILED: Value access (const Value() != 123)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Value access");
				}
			}
		}

		// Value mutation
		{
			auto r = Result<UINT32, UINT32>::Ok(100);
			r.Value() = 200;
			if (r.Value() != 200)
			{
				LOG_ERROR("  FAILED: Value mutation (Value() != 200, got %u)", r.Value());
				allPassed = false;
			}
			else
			{
				LOG_INFO("  PASSED: Value mutation");
			}
		}

		return allPassed;
	}

	// =====================================================================
	// Move semantics suite
	// =====================================================================

	static BOOL TestMoveSemanticsSuite()
	{
		BOOL allPassed = true;

		// Move construction
		{
			// Move Ok
			auto ok1 = Result<UINT32, UINT32>::Ok(42);
			auto ok2 = static_cast<Result<UINT32, UINT32> &&>(ok1);
			if (!ok2.IsOk() || ok2.Value() != 42)
			{
				LOG_ERROR("  FAILED: Move construction (Ok value mismatch after move)");
				allPassed = false;
			}
			else
			{
				// Move Err
				auto err1 = Result<UINT32, Error>::Err(Error::Socket_OpenFailed_Connect);
				auto err2 = static_cast<Result<UINT32, Error> &&>(err1);
				if (!err2.IsErr())
				{
					LOG_ERROR("  FAILED: Move construction (Err IsErr() false after move)");
					allPassed = false;
				}
				else if (err2.Error().Code != Error::Socket_OpenFailed_Connect)
				{
					LOG_ERROR("  FAILED: Move construction (Err code mismatch after move)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Move construction");
				}
			}
		}

		// Move assignment
		{
			auto r = Result<UINT32, UINT32>::Ok(10);
			if (!r.IsOk() || r.Value() != 10)
			{
				LOG_ERROR("  FAILED: Move assignment (initial Ok(10) check failed)");
				allPassed = false;
			}
			else
			{
				// Reassign from Err
				r = Result<UINT32, UINT32>::Err(20);
				if (!r.IsErr())
				{
					LOG_ERROR("  FAILED: Move assignment (after reassign to Err: IsErr() false)");
					allPassed = false;
				}
				else
				{
					// Reassign back to Ok
					r = Result<UINT32, UINT32>::Ok(30);
					if (!r.IsOk() || r.Value() != 30)
					{
						LOG_ERROR("  FAILED: Move assignment (after reassign to Ok(30): check failed)");
						allPassed = false;
					}
					else
					{
						LOG_INFO("  PASSED: Move assignment");
					}
				}
			}
		}

		// Void move construction
		{
			auto ok1 = Result<VOID, UINT32>::Ok();
			auto ok2 = static_cast<Result<VOID, UINT32> &&>(ok1);
			if (!ok2.IsOk())
			{
				LOG_ERROR("  FAILED: Void move construction (Ok IsOk() false)");
				allPassed = false;
			}
			else
			{
				auto err1 = Result<VOID, UINT32>::Err(7);
				auto err2 = static_cast<Result<VOID, UINT32> &&>(err1);
				if (!err2.IsErr())
				{
					LOG_ERROR("  FAILED: Void move construction (Err IsErr() false)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Void move construction");
				}
			}
		}

		// Non-trivial destructor
		{
			BOOL destroyed = false;
			{
				auto r = Result<Tracked, UINT32>::Ok(Tracked(1, &destroyed));
				if (destroyed)
				{
					LOG_ERROR("  FAILED: Non-trivial destructor (destroyed prematurely inside scope)");
					allPassed = false;
				}
			}
			if (allPassed)
			{
				if (!destroyed)
				{
					LOG_ERROR("  FAILED: Non-trivial destructor (not destroyed after scope exit)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Non-trivial destructor");
				}
			}
		}

		// Move transfers ownership
		{
			BOOL destroyed = false;
			{
				auto r1 = Result<Tracked, UINT32>::Ok(Tracked(3, &destroyed));
				{
					auto r2 = static_cast<Result<Tracked, UINT32> &&>(r1);
					if (destroyed)
					{
						LOG_ERROR("  FAILED: Move transfers ownership (destroyed after move, r2 still alive)");
						allPassed = false;
					}
					else if (r2.Value().value != 3)
					{
						LOG_ERROR("  FAILED: Move transfers ownership (value mismatch: expected 3)");
						allPassed = false;
					}
				}
				// r2 out of scope — destructor fires
				if (allPassed && !destroyed)
				{
					LOG_ERROR("  FAILED: Move transfers ownership (not destroyed after r2 scope exit)");
					allPassed = false;
				}
			}
			// r1 source was nullified by move — no double-destroy
			if (allPassed)
			{
				LOG_INFO("  PASSED: Move transfers ownership");
			}
		}

		return allPassed;
	}

	// =====================================================================
	// Error chaining suite
	// =====================================================================

	static BOOL TestErrorChainingSuite()
	{
		BOOL allPassed = true;

		// Single error storage
		{
			auto r = Result<UINT32, Error>::Err(Error::Dns_ConnectFailed);
			if (!r.IsErr())
			{
				LOG_ERROR("  FAILED: Single error storage (IsErr() false)");
				allPassed = false;
			}
			else
			{
				const Error &err = r.Error();
				if (err.Code != Error::Dns_ConnectFailed)
				{
					LOG_ERROR("  FAILED: Single error storage (Code mismatch)");
					allPassed = false;
				}
				else if (err.Platform != Error::PlatformKind::Runtime)
				{
					LOG_ERROR("  FAILED: Single error storage (Platform mismatch)");
					allPassed = false;
				}
				else if (err.Depth != 0)
				{
					LOG_ERROR("  FAILED: Single error storage (Depth should be 0)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Single error storage");
				}
			}
		}

		// Two-arg Err chaining
		{
			auto r = Result<UINT32, Error>::Err(
				Error::Windows(0xC0000034),
				Error::Socket_OpenFailed_Connect);
			if (!r.IsErr())
			{
				LOG_ERROR("  FAILED: Two-arg Err chaining (IsErr() false)");
				allPassed = false;
			}
			else
			{
				const Error &err = r.Error();
				if (err.Code != Error::Socket_OpenFailed_Connect)
				{
					LOG_ERROR("  FAILED: Two-arg Err chaining (Code mismatch, expected Socket_OpenFailed_Connect, got %u)",
						(UINT32)err.Code);
					allPassed = false;
				}
				else if (err.Platform != Error::PlatformKind::Runtime)
				{
					LOG_ERROR("  FAILED: Two-arg Err chaining (Platform mismatch)");
					allPassed = false;
				}
				else if (err.Depth != 1)
				{
					LOG_ERROR("  FAILED: Two-arg Err chaining (Depth mismatch, expected 1, got %u)", (UINT32)err.Depth);
					allPassed = false;
				}
				else if (err.InnerCodes[0] != 0xC0000034)
				{
					LOG_ERROR("  FAILED: Two-arg Err chaining (InnerCodes[0] mismatch)");
					allPassed = false;
				}
				else if (err.InnerPlatforms[0] != Error::PlatformKind::Windows)
				{
					LOG_ERROR("  FAILED: Two-arg Err chaining (InnerPlatforms[0] mismatch)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Two-arg Err chaining");
				}
			}
		}

		// Propagation Err chaining
		{
			auto inner = Result<UINT32, Error>::Err(
				Error::Posix(111),
				Error::Socket_WriteFailed_Send);

			auto outer = Result<VOID, Error>::Err(inner, Error::Tls_WriteFailed_Send);
			if (!outer.IsErr())
			{
				LOG_ERROR("  FAILED: Propagation Err chaining (IsErr() false)");
				allPassed = false;
			}
			else
			{
				const Error &err = outer.Error();
				if (err.Code != Error::Tls_WriteFailed_Send)
				{
					LOG_ERROR("  FAILED: Propagation Err chaining (Code mismatch, expected Tls_WriteFailed_Send, got %u)",
						(UINT32)err.Code);
					allPassed = false;
				}
				else if (err.Platform != Error::PlatformKind::Runtime)
				{
					LOG_ERROR("  FAILED: Propagation Err chaining (Platform mismatch)");
					allPassed = false;
				}
				else if (err.Depth != 2)
				{
					LOG_ERROR("  FAILED: Propagation Err chaining (Depth mismatch, expected 2, got %u)", (UINT32)err.Depth);
					allPassed = false;
				}
				else if (err.InnerCodes[0] != (UINT32)Error::Socket_WriteFailed_Send)
				{
					LOG_ERROR("  FAILED: Propagation Err chaining (InnerCodes[0] mismatch)");
					allPassed = false;
				}
				else if (err.InnerPlatforms[0] != Error::PlatformKind::Runtime)
				{
					LOG_ERROR("  FAILED: Propagation Err chaining (InnerPlatforms[0] mismatch)");
					allPassed = false;
				}
				else if (err.InnerCodes[1] != 111)
				{
					LOG_ERROR("  FAILED: Propagation Err chaining (InnerCodes[1] mismatch)");
					allPassed = false;
				}
				else if (err.InnerPlatforms[1] != Error::PlatformKind::Posix)
				{
					LOG_ERROR("  FAILED: Propagation Err chaining (InnerPlatforms[1] mismatch)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Propagation Err chaining");
				}
			}
		}

		// Multi-level error chaining
		{
			// Simulate: OS -> Socket -> TLS -> HTTP (4 levels)
			auto osResult = Result<VOID, Error>::Err(Error::Windows(0xC0000034));
			auto socketResult = Result<VOID, Error>::Err(osResult, Error::Socket_OpenFailed_Connect);
			auto tlsResult = Result<VOID, Error>::Err(socketResult, Error::Tls_OpenFailed_Socket);
			auto httpResult = Result<VOID, Error>::Err(tlsResult, Error::Http_OpenFailed);

			const Error &err = httpResult.Error();

			if (err.Code != Error::Http_OpenFailed)
			{
				LOG_ERROR("  FAILED: Multi-level chaining (Code mismatch, expected Http_OpenFailed)");
				allPassed = false;
			}
			else if (err.Depth != 3)
			{
				LOG_ERROR("  FAILED: Multi-level chaining (Depth mismatch, expected 3, got %u)", (UINT32)err.Depth);
				allPassed = false;
			}
			else if (err.InnerCodes[0] != (UINT32)Error::Tls_OpenFailed_Socket)
			{
				LOG_ERROR("  FAILED: Multi-level chaining (InnerCodes[0] mismatch)");
				allPassed = false;
			}
			else if (err.InnerCodes[1] != (UINT32)Error::Socket_OpenFailed_Connect)
			{
				LOG_ERROR("  FAILED: Multi-level chaining (InnerCodes[1] mismatch)");
				allPassed = false;
			}
			else if (err.InnerCodes[2] != 0xC0000034)
			{
				LOG_ERROR("  FAILED: Multi-level chaining (InnerCodes[2] mismatch)");
				allPassed = false;
			}
			else if (err.InnerPlatforms[2] != Error::PlatformKind::Windows)
			{
				LOG_ERROR("  FAILED: Multi-level chaining (InnerPlatforms[2] mismatch)");
				allPassed = false;
			}
			else if (err.RootCode() != 0xC0000034)
			{
				LOG_ERROR("  FAILED: Multi-level chaining (RootCode mismatch)");
				allPassed = false;
			}
			else if (err.RootPlatform() != Error::PlatformKind::Windows)
			{
				LOG_ERROR("  FAILED: Multi-level chaining (RootPlatform mismatch)");
				allPassed = false;
			}
			else if (err.TotalDepth() != 4)
			{
				LOG_ERROR("  FAILED: Multi-level chaining (TotalDepth mismatch, expected 4)");
				allPassed = false;
			}
			else
			{
				LOG_INFO("  PASSED: Multi-level error chaining");
			}
		}

		return allPassed;
	}

	// =====================================================================
	// Compact specialization suite
	// =====================================================================

	static BOOL TestCompactSpecializationSuite()
	{
		BOOL allPassed = true;

		// Compact size
		{
			static_assert(sizeof(Result<VOID, Error>) == sizeof(Error),
				"Compact specialization must equal sizeof(Error)");
			static_assert(sizeof(Result<VOID, UINT32>) > sizeof(UINT32),
				"Primary template Result<VOID, UINT32> should have m_isOk overhead");
			LOG_INFO("  PASSED: Compact specialization size");
		}

		// Compact void Ok
		{
			auto r = Result<VOID, Error>::Ok();
			if (!r.IsOk())
			{
				LOG_ERROR("  FAILED: Compact void Ok (IsOk() returned false)");
				allPassed = false;
			}
			else if (r.IsErr())
			{
				LOG_ERROR("  FAILED: Compact void Ok (IsErr() returned true)");
				allPassed = false;
			}
			else if (!r)
			{
				LOG_ERROR("  FAILED: Compact void Ok (operator BOOL returned false)");
				allPassed = false;
			}
			else
			{
				LOG_INFO("  PASSED: Compact void Ok");
			}
		}

		// Compact void Err
		{
			auto r = Result<VOID, Error>::Err(Error::Socket_CreateFailed_Open);
			if (!r.IsErr())
			{
				LOG_ERROR("  FAILED: Compact void Err (IsErr() returned false)");
				allPassed = false;
			}
			else if (r.IsOk())
			{
				LOG_ERROR("  FAILED: Compact void Err (IsOk() returned true)");
				allPassed = false;
			}
			else if (r)
			{
				LOG_ERROR("  FAILED: Compact void Err (operator BOOL returned true)");
				allPassed = false;
			}
			else
			{
				const Error &err = r.Error();
				if (err.Code != Error::Socket_CreateFailed_Open)
				{
					LOG_ERROR("  FAILED: Compact void Err (Code mismatch)");
					allPassed = false;
				}
				else if (err.Platform != Error::PlatformKind::Runtime)
				{
					LOG_ERROR("  FAILED: Compact void Err (Platform mismatch)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Compact void Err");
				}
			}
		}

		// Compact void propagation Err
		{
			auto inner = Result<UINT32, Error>::Err(
				Error::Posix(111),
				Error::Socket_WriteFailed_Send);

			auto outer = Result<VOID, Error>::Err(inner, Error::Tls_WriteFailed_Send);
			if (!outer.IsErr())
			{
				LOG_ERROR("  FAILED: Compact void propagation (IsErr() false)");
				allPassed = false;
			}
			else
			{
				const Error &err = outer.Error();
				if (err.Code != Error::Tls_WriteFailed_Send)
				{
					LOG_ERROR("  FAILED: Compact void propagation (Code mismatch, expected Tls_WriteFailed_Send, got %u)",
						(UINT32)err.Code);
					allPassed = false;
				}
				else if (err.Platform != Error::PlatformKind::Runtime)
				{
					LOG_ERROR("  FAILED: Compact void propagation (Platform mismatch)");
					allPassed = false;
				}
				else if (err.Depth != 2)
				{
					LOG_ERROR("  FAILED: Compact void propagation (Depth mismatch, expected 2, got %u)", (UINT32)err.Depth);
					allPassed = false;
				}
				else if (err.InnerCodes[0] != (UINT32)Error::Socket_WriteFailed_Send)
				{
					LOG_ERROR("  FAILED: Compact void propagation (InnerCodes[0] mismatch)");
					allPassed = false;
				}
				else if (err.InnerCodes[1] != 111)
				{
					LOG_ERROR("  FAILED: Compact void propagation (InnerCodes[1] mismatch)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Compact void propagation Err");
				}
			}
		}

		// Compact void two-arg Err
		{
			auto r = Result<VOID, Error>::Err(
				Error::Windows(0xC0000034),
				Error::Socket_OpenFailed_Connect);
			if (!r.IsErr())
			{
				LOG_ERROR("  FAILED: Compact void two-arg Err (IsErr() false)");
				allPassed = false;
			}
			else
			{
				const Error &err = r.Error();
				if (err.Code != Error::Socket_OpenFailed_Connect)
				{
					LOG_ERROR("  FAILED: Compact void two-arg Err (Code mismatch, expected Socket_OpenFailed_Connect, got %u)",
						(UINT32)err.Code);
					allPassed = false;
				}
				else if (err.Platform != Error::PlatformKind::Runtime)
				{
					LOG_ERROR("  FAILED: Compact void two-arg Err (Platform mismatch)");
					allPassed = false;
				}
				else if (err.Depth != 1)
				{
					LOG_ERROR("  FAILED: Compact void two-arg Err (Depth mismatch, expected 1, got %u)", (UINT32)err.Depth);
					allPassed = false;
				}
				else if (err.InnerCodes[0] != 0xC0000034)
				{
					LOG_ERROR("  FAILED: Compact void two-arg Err (InnerCodes[0] mismatch)");
					allPassed = false;
				}
				else if (err.InnerPlatforms[0] != Error::PlatformKind::Windows)
				{
					LOG_ERROR("  FAILED: Compact void two-arg Err (InnerPlatforms[0] mismatch)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Compact void two-arg Err");
				}
			}
		}

		// Compact Error() on Ok is well-defined
		{
			auto r = Result<VOID, Error>::Ok();
			const Error &err = r.Error();
			if (err.Code != Error::None)
			{
				LOG_ERROR("  FAILED: Compact Error() on Ok (Code != None)");
				allPassed = false;
			}
			else if (err.Platform != Error::PlatformKind::Runtime)
			{
				LOG_ERROR("  FAILED: Compact Error() on Ok (Platform != Runtime)");
				allPassed = false;
			}
			else
			{
				LOG_INFO("  PASSED: Compact Error() on Ok is well-defined");
			}
		}

		// Compact void move construction
		{
			// Move Ok
			auto ok1 = Result<VOID, Error>::Ok();
			auto ok2 = static_cast<Result<VOID, Error> &&>(ok1);
			if (!ok2.IsOk())
			{
				LOG_ERROR("  FAILED: Compact void move construction (Ok IsOk() false)");
				allPassed = false;
			}
			else
			{
				// Move Err
				auto err1 = Result<VOID, Error>::Err(Error::Dns_ConnectFailed);
				auto err2 = static_cast<Result<VOID, Error> &&>(err1);
				if (!err2.IsErr())
				{
					LOG_ERROR("  FAILED: Compact void move construction (Err IsErr() false)");
					allPassed = false;
				}
				else if (err2.Error().Code != Error::Dns_ConnectFailed)
				{
					LOG_ERROR("  FAILED: Compact void move construction (Code mismatch after move)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Compact void move construction");
				}
			}
		}

		return allPassed;
	}

	// =====================================================================
	// Error accessors suite
	// =====================================================================

	static BOOL TestErrorAccessorsSuite()
	{
		BOOL allPassed = true;

		// Root cause accessors
		{
			// Single error — root is itself
			Error single(Error::Socket_CreateFailed_Open);
			if (single.RootCode() != (UINT32)Error::Socket_CreateFailed_Open)
			{
				LOG_ERROR("  FAILED: Root cause accessors (single error RootCode mismatch)");
				allPassed = false;
			}
			else if (single.RootPlatform() != Error::PlatformKind::Runtime)
			{
				LOG_ERROR("  FAILED: Root cause accessors (single error RootPlatform mismatch)");
				allPassed = false;
			}
			else if (single.TotalDepth() != 1)
			{
				LOG_ERROR("  FAILED: Root cause accessors (single error TotalDepth mismatch)");
				allPassed = false;
			}
			else
			{
				// Chained error — root is the innermost
				Error chained = Error::Wrap(Error::Windows(0xC0000001), Error::Socket_CreateFailed_Open);
				if (chained.RootCode() != 0xC0000001)
				{
					LOG_ERROR("  FAILED: Root cause accessors (chained RootCode mismatch)");
					allPassed = false;
				}
				else if (chained.RootPlatform() != Error::PlatformKind::Windows)
				{
					LOG_ERROR("  FAILED: Root cause accessors (chained RootPlatform mismatch)");
					allPassed = false;
				}
				else if (chained.TotalDepth() != 2)
				{
					LOG_ERROR("  FAILED: Root cause accessors (chained TotalDepth mismatch)");
					allPassed = false;
				}
				else
				{
					LOG_INFO("  PASSED: Error root cause accessors");
				}
			}
		}

		// Chain overflow truncation
		{
			Error err(Error::Socket_CreateFailed_Open);
			for (UINT32 i = 0; i < Error::MaxDepth + 2; i++)
			{
				err = Error::Wrap(err, 100 + i);
			}

			if (err.Depth > Error::MaxInnerDepth)
			{
				LOG_ERROR("  FAILED: Chain overflow (Depth %u exceeds MaxInnerDepth %u)",
					(UINT32)err.Depth, (UINT32)Error::MaxInnerDepth);
				allPassed = false;
			}
			else if (err.Code == Error::None)
			{
				LOG_ERROR("  FAILED: Chain overflow (Code is None after overflow)");
				allPassed = false;
			}
			else
			{
				LOG_INFO("  PASSED: Error chain overflow truncation");
			}
		}

		// Non-chainable E type
		{
			auto r1 = Result<UINT32, UINT32>::Err(42);
			if (!r1.IsErr())
			{
				LOG_ERROR("  FAILED: Non-chainable E type (Err(42) IsErr() false)");
				allPassed = false;
			}
			else if (r1.IsOk())
			{
				LOG_ERROR("  FAILED: Non-chainable E type (Err(42) IsOk() true)");
				allPassed = false;
			}
			else
			{
				auto r2 = Result<VOID, UINT32>::Err(7);
				if (!r2.IsErr())
				{
					LOG_ERROR("  FAILED: Non-chainable E type (void Err(7) IsErr() false)");
					allPassed = false;
				}
				else
				{
					auto r3 = Result<UINT32, UINT32>::Ok(100);
					if (!r3.IsOk() || r3.Value() != 100)
					{
						LOG_ERROR("  FAILED: Non-chainable E type (Ok(100) check failed)");
						allPassed = false;
					}
					else
					{
						LOG_INFO("  PASSED: Non-chainable E type");
					}
				}
			}
		}

		// Type aliases
		{
			static_assert(__is_same(Result<UINT32, UINT64>::ValueType, UINT32));
			static_assert(__is_same(Result<UINT32, UINT64>::ErrorType, UINT64));
			static_assert(__is_same(Result<VOID, UINT32>::ValueType, VOID));
			static_assert(__is_same(Result<VOID, UINT32>::ErrorType, UINT32));
			LOG_INFO("  PASSED: Type aliases");
		}

		return allPassed;
	}
};
