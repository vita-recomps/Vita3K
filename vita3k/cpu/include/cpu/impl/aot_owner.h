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

// The CPU recompiled code runs on (cpu/aot.h): what the hooks native code calls (aot_hooks.h)
// dispatch to. DynarmicCPU and NativeCPU implement it.
#pragma once

#include <cpu/aot.h>

#include <cstdint>

class AotOwner {
public:
    virtual ~AotOwner() = default;
    // native code calling native code (call_indirect)
    virtual void aot_call_native(uint32_t addr, AotFn fn) = 0;
    // native code calling code that has no native function
    virtual void aot_call_guest(uint32_t target) = 0;
    // native code calling an import stub
    virtual void aot_call_import(uint32_t stub) = 0;
};

// Native code jumped somewhere it cannot continue (aot_hooks::escape): the CPU continues from pc
// and the native frames since the last boundary are abandoned.
struct AotEscape {
    uint32_t pc;
};

// The thread is exiting (CPUState::aot_unwind): abandon every native frame up to run().
struct AotUnwind {};
