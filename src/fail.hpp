// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Internal checks of code whose input is verified before it runs (the MU ROM converter: only
// ROM images whose CRC-32 is that of a known dump reach it). With exceptions they throw, as
// before; built without exceptions (-fno-exceptions, an embedded host) a failed check aborts,
// which cannot happen for the verified input. Errors that depend on what a caller gives (bank
// tables, DLLs) are reported as hvyxg::Error instead, never through these.

#include <cstdlib>

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#define HVYXG_EXCEPTIONS 1
#define HVYXG_THROW(...) throw __VA_ARGS__
#else
#define HVYXG_EXCEPTIONS 0
#define HVYXG_THROW(...) ::std::abort()
#endif
