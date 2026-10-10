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

#pragma once

#include <cpu/common.h>
#include <cpu/disasm/state.h>
#include <mem/state.h>
#include <util/types.h>

#include <atomic>
#include <functional>

struct CPUState {
    CPUState() = default;

    SceUID thread_id = 0;
    MemState *mem = nullptr;
    DisasmState disasm;

    CPUInterfacePtr cpu;
    bool svc_called;
    uint32_t svc;

    // Recompiled code (cpu/aot.h). Native code calls HLE imports itself, through this handler,
    // instead of returning to the thread's run loop; aot_unwind asks it to abandon its frames
    // because the thread is exiting.
    std::function<void(CPUState &, uint32_t nid)> import_handler;
    std::atomic<bool> aot_unwind{ false };

    // Exception handler support (kubridge abort handlers)
    // These are set by the page fault callback (signal-safe atomics)
    // and consumed by run_loop after HaltExecution returns.
    std::atomic<bool> abort_pending{ false };
    std::atomic<uint32_t> abort_fault_addr{ 0 };
    std::atomic<bool> abort_is_write{ false };
};
