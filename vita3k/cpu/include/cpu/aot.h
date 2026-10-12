// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// Ahead-of-time recompiled guest code (ss-delta). Modules recompiled to C++ and built as DLLs
// are listed in the SSD_AOT environment variable (';'-separated paths). The JIT stops at the
// entry of every function such a module provides and the function runs natively instead; native
// code calls back into the JIT for everything else (see dynarmic_cpu.cpp).
#pragma once

#include <mem/util.h>

#include <cstdint>
#include <string>

struct MemState;

namespace armrecomp {
struct GuestContext;
}

using AotFn = void (*)(armrecomp::GuestContext *);

// Load the modules named in SSD_AOT (once per process) and bind them to this guest memory.
// sentinel is a guest address no code runs from: native code calling into the JIT returns there.
void aot_init(MemState &mem, Address sentinel);
void aot_deinit();

bool aot_enabled();
Address aot_sentinel();
// the kernel's halt instruction (guest code ends a thread or a callback by returning to it)
void aot_set_halt(Address halt);
Address aot_halt();
// SSD_AOT_NATIVE=1 with recompiled modules: threads run on NativeCPU, without the JIT
bool aot_native_cpu();

// Where a module's native code expects its code segment: the page of its first function, found by
// the name of the ELF it was generated from without the extension (e.g. "adhoc_matching"), or 0.
// The loader places a relocatable module there when that memory is free, so that a module loaded
// late in a session (ad hoc play) lands where its code was generated for (docs/decisions.md D8).
Address aot_preferred_base(const std::string &module_stem);

// The native function for a guest entry address (bit 0 set for Thumb), or null. A module's
// functions become visible once its code has been checked against guest memory.
AotFn aot_lookup(uint32_t addr);
// the function containing a resume point (aot_abi.h AotResume), to call with GuestContext::resume set
AotFn aot_lookup_resume(uint32_t addr);
void aot_count_resume_entry();

// Counters, logged every 10 s while native code runs and at deinit:
// JIT -> native entries, native -> JIT calls (call_indirect, LLE import stubs), native -> HLE
// calls, escapes (native -> JIT, abandoning native frames) and, with SSD_AOT_COUNT=1, the guest
// instructions each side ran (the JIT through dynarmic's cycle counting, native code through
// GuestContext::insns in modules built with RECOMP_COUNT_INSNS).
bool aot_count_jit_insns();
void aot_add_jit_insns(uint64_t n);
void aot_add_native_insns(uint64_t n);
void aot_count_native_entry();
// Differential mode: SSD_AOT_DIFF=N checks every Nth native call per thread against the JIT
uint64_t aot_diff_interval();
void aot_count_diff(uint32_t pc, bool checked, bool mismatch, const char *skip_reason);
void aot_count_diff_cost(uint64_t microseconds, uint64_t insns);
void aot_note_mismatch(const std::string &text); // the last few appear in the counters text
// JIT instructions run for a native -> JIT call to `key` (called) or from a top-level JIT run that
// started at `key`; the counters text lists the heaviest
void aot_profile_jit(uint32_t key, uint64_t insns, bool called);
void aot_count_guest_call();
void aot_count_import_call();
void aot_count_escape(uint32_t pc);
std::string aot_counters_text();
