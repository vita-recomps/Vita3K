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

// A CPU without a JIT: every guest function it runs is recompiled code (cpu/aot.h). The registers
// live in the native context; run() calls the native function at pc until the guest returns to the
// kernel's halt instruction. Guest code without native code is an error. (ss-delta, SSD_AOT_NATIVE)
#pragma once

#include <cpu/aot_abi.h>
#include <cpu/common.h>
#include <cpu/impl/aot_owner.h>
#include <cpu/impl/interface.h>

class NativeCPU : public CPUInterface, public AotOwner {
    CPUState *parent;
    std::size_t core_id;
    armrecomp::GuestContext guest{};
    uint32_t cpsr_rest = 0x10; // mode and the T bit; the flags live in the native context
    bool halted = false;
    bool log_code = false;
    bool log_mem = false;

    [[noreturn]] void missing_native_code(uint32_t pc);

public:
    NativeCPU(CPUState *state, std::size_t processor_id);

    int run() override;
    void stop() override;

    uint32_t get_reg(uint8_t idx) override;
    void set_reg(uint8_t idx, uint32_t val) override;
    uint32_t get_sp() override;
    void set_sp(uint32_t val) override;
    uint32_t get_pc() override;
    void set_pc(uint32_t val) override;
    uint32_t get_lr() override;
    void set_lr(uint32_t val) override;
    uint32_t get_cpsr() override;
    void set_cpsr(uint32_t val) override;
    uint32_t get_tpidruro() override;
    void set_tpidruro(uint32_t val) override;
    float get_float_reg(uint8_t idx) override;
    void set_float_reg(uint8_t idx, float val) override;
    uint32_t get_fpscr() override;
    void set_fpscr(uint32_t val) override;

    CPUContext save_context() override;
    void load_context(const CPUContext &ctx) override;
    void invalidate_jit_cache(Address start, size_t length) override;

    bool is_thumb_mode() override;
    int step() override;

    bool hit_breakpoint() override;
    void trigger_breakpoint() override;
    void set_log_code(bool log) override;
    void set_log_mem(bool log) override;
    bool get_log_code() override;
    bool get_log_mem() override;

    void clear_exclusive() override;
    std::size_t processor_id() const override;

    void aot_call_native(uint32_t addr, AotFn fn) override;
    void aot_call_guest(uint32_t target) override;
    void aot_call_import(uint32_t stub) override;
};
