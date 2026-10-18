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

#include <cpu/impl/native_cpu.h>

#include <cpu/state.h>
#include <mem/ptr.h>
#include <util/log.h>

#include <bit>
#include <cstring>
#include <mutex>
#include <set>

NativeCPU::NativeCPU(CPUState *state, std::size_t processor_id)
    : parent(state)
    , core_id(processor_id) {
    guest.embedder = static_cast<AotOwner *>(this);
}

void NativeCPU::missing_native_code(uint32_t pc) {
    static std::mutex m;
    static std::set<uint32_t> reported;
    {
        const std::lock_guard<std::mutex> lock(m);
        if (reported.insert(pc).second)
            LOG_CRITICAL("NativeCPU: no native code for the guest code at {} (thread {}). It needs recompiling; meanwhile the game's .ini [recompiled] jit = true runs it on the JIT (a build with dynarmic)", log_hex(pc), parent->thread_id);
    }
    throw AotUnwind{};
}

// Calls the native function at pc until the guest returns to the halt instruction; returns 1 then,
// -1 when guest code has no native code
int NativeCPU::run() {
    halted = false;
    parent->svc_called = false;
    const uint32_t halt = aot_halt();
    try {
        while (true) {
            const uint32_t pc = guest.r[15];
            const bool thumb = is_thumb_mode();
            if (pc == halt && thumb) {
                halted = true;
                return 1;
            }
            const uint32_t entry = pc | (thumb ? 1u : 0u);
            AotFn fn = aot_lookup(entry);
            if (fn) {
                aot_count_native_entry();
            } else if ((fn = aot_lookup_resume(entry))) {
                aot_count_native_entry();
                aot_count_resume_entry();
                guest.resume = pc;
            } else {
                missing_native_code(pc);
            }
            uint32_t next;
            try {
                fn(&guest);
                next = guest.r[15];
            } catch (const AotEscape &e) {
                next = e.pc;
            }
            aot_add_native_insns(guest.insns);
            guest.insns = 0;
            set_pc(next);
        }
    } catch (const AotUnwind &) {
        // the thread is exiting (its run loop sees why), or guest code had no native code
        if (!parent->aot_unwind)
            return -1;
    }
    return halted;
}

// native code is not preemptible: suspend requests take effect when it returns
void NativeCPU::stop() {}

void NativeCPU::aot_call_native(uint32_t, AotFn fn) {
    fn(&guest);
}

void NativeCPU::aot_call_guest(uint32_t target) {
    AotFn fn = aot_lookup(target);
    if (!fn) {
        // a call into a function's middle, at one of its resume points (a hand-written routine's
        // helper reached through a computed address): the function starts there
        fn = aot_lookup_resume(target);
        if (!fn)
            missing_native_code(target & ~1u);
        aot_count_resume_entry();
        guest.resume = target & ~1u;
    }
    const uint32_t ret = guest.r[14];
    fn(&guest);
    guest.r[15] = ret;
}

// As DynarmicCPU::aot_call_import, with the registers already in the native context
void NativeCPU::aot_call_import(uint32_t stub) {
    const uint32_t *const words = Ptr<uint32_t>(stub).get(*parent->mem);
    if (words[0] != 0xef000000 || words[1] != 0xe1a0f00e) {
        // bound to another module's export: movw r12, #lo; movt r12, #hi; bx r12
        if ((words[0] & 0x0ff0f000) == 0x0300c000 && (words[1] & 0x0ff0f000) == 0x0340c000 && (words[2] & 0x0fffffff) == 0x012fff1c) {
            const uint32_t lo = ((words[0] >> 4) & 0xf000) | (words[0] & 0xfff);
            const uint32_t hi = ((words[1] >> 4) & 0xf000) | (words[1] & 0xfff);
            const uint32_t target = (hi << 16) | lo;
            if (const AotFn fn = aot_lookup(target)) {
                guest.r[12] = target;
                fn(&guest);
                return;
            }
            missing_native_code(target & ~1u);
        }
        missing_native_code(stub);
    }
    // svc #0; mov pc, lr; .word nid: the HLE function, as the run loop would call it
    aot_count_import_call();
    const uint32_t ret = guest.r[14], sp = guest.r[13];
    set_pc(stub + 4);
    const bool was_halted = halted;
    parent->import_handler(*parent, words[2]);
    guest.excl_valid = 0;
    halted = was_halted;
    if (parent->aot_unwind)
        throw AotUnwind{};
    if (guest.r[15] != stub + 4 || is_thumb_mode() || guest.r[13] != sp || guest.r[14] != ret) {
        // the HLE function switched contexts (SceFiber): continue where the other context is
        aot_count_escape(stub);
        if (!is_thumb_mode() && *Ptr<uint32_t>(guest.r[15]).get(*parent->mem) == 0xe1a0f00e)
            throw AotEscape{ guest.r[14] };
        throw AotEscape{ guest.r[15] | (is_thumb_mode() ? 1u : 0u) };
    }
    guest.r[15] = ret;
}

uint32_t NativeCPU::get_reg(uint8_t idx) {
    return guest.r[idx];
}

void NativeCPU::set_reg(uint8_t idx, uint32_t val) {
    guest.r[idx] = val;
}

uint32_t NativeCPU::get_sp() {
    return guest.r[13];
}

void NativeCPU::set_sp(uint32_t val) {
    guest.r[13] = val;
}

uint32_t NativeCPU::get_pc() {
    return guest.r[15];
}

void NativeCPU::set_pc(uint32_t val) {
    if (val & 1) {
        cpsr_rest |= 0x20;
        guest.r[15] = val & ~1u;
    } else {
        cpsr_rest &= ~0x20u;
        guest.r[15] = val & ~3u;
    }
}

uint32_t NativeCPU::get_lr() {
    return guest.r[14];
}

void NativeCPU::set_lr(uint32_t val) {
    guest.r[14] = val;
}

uint32_t NativeCPU::get_cpsr() {
    return guest.n << 31 | guest.z << 30 | guest.c << 29 | guest.v << 28 | guest.q << 27 | (guest.ge & 0xf) << 16 | (cpsr_rest & 0x07f0ffffu);
}

void NativeCPU::set_cpsr(uint32_t val) {
    guest.n = (val >> 31) & 1;
    guest.z = (val >> 30) & 1;
    guest.c = (val >> 29) & 1;
    guest.v = (val >> 28) & 1;
    guest.q = (val >> 27) & 1;
    guest.ge = (val >> 16) & 0xf;
    cpsr_rest = val & 0x07f0ffffu;
}

uint32_t NativeCPU::get_tpidruro() {
    return guest.tpidruro;
}

void NativeCPU::set_tpidruro(uint32_t val) {
    guest.tpidruro = val;
}

float NativeCPU::get_float_reg(uint8_t idx) {
    return guest.vfp.f[idx];
}

void NativeCPU::set_float_reg(uint8_t idx, float val) {
    guest.vfp.f[idx] = val;
}

uint32_t NativeCPU::get_fpscr() {
    return guest.fpscr;
}

void NativeCPU::set_fpscr(uint32_t val) {
    guest.fpscr = val;
}

CPUContext NativeCPU::save_context() {
    CPUContext ctx;
    std::copy(guest.r, guest.r + 16, ctx.cpu_registers.begin());
    static_assert(sizeof(ctx.fpu_registers) == sizeof(guest.vfp));
    memcpy(ctx.fpu_registers.data(), guest.vfp.s, sizeof(ctx.fpu_registers));
    ctx.fpscr = guest.fpscr;
    ctx.cpsr = get_cpsr();
    return ctx;
}

void NativeCPU::load_context(const CPUContext &ctx) {
    std::copy(ctx.cpu_registers.begin(), ctx.cpu_registers.end(), guest.r);
    memcpy(guest.vfp.s, ctx.fpu_registers.data(), sizeof(guest.vfp));
    guest.fpscr = ctx.fpscr;
    set_cpsr(ctx.cpsr);
}

void NativeCPU::invalidate_jit_cache(Address, size_t) {}

bool NativeCPU::is_thumb_mode() {
    return cpsr_rest & 0x20;
}

// no single stepping without a JIT: run instead
int NativeCPU::step() {
    return run();
}

bool NativeCPU::hit_breakpoint() {
    return false;
}

void NativeCPU::trigger_breakpoint() {}

void NativeCPU::set_log_code(bool log) {
    log_code = log;
}

void NativeCPU::set_log_mem(bool log) {
    log_mem = log;
}

bool NativeCPU::get_log_code() {
    return log_code;
}

bool NativeCPU::get_log_mem() {
    return log_mem;
}

void NativeCPU::clear_exclusive() {
    guest.excl_valid = 0;
}

std::size_t NativeCPU::processor_id() const {
    return core_id;
}
