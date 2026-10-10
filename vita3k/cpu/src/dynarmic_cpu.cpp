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

#include "cpu/common.h"
#include <cpu/aot.h>
#include <cpu/impl/aot_hooks.h>
#include <cpu/impl/dynarmic_cpu.h>
#include <cpu/state.h>
#include <util/log.h>

#include <mem/ptr.h>

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/backend/x64/exclusive_monitor_friend.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/ir/terminal.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

class ArmDynarmicCP15 : public Dynarmic::A32::Coprocessor {
    uint32_t tpidruro;
    uint32_t sctlr;
    uint32_t dacr;

public:
    using CoprocReg = Dynarmic::A32::CoprocReg;

    explicit ArmDynarmicCP15()
        : tpidruro(0)
        , sctlr(0)
        , dacr(0) {
    }

    ~ArmDynarmicCP15() override = default;

    std::optional<Callback> CompileInternalOperation(bool two, unsigned opc1, CoprocReg CRd,
        CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        return std::nullopt;
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn,
        CoprocReg CRm, unsigned opc2) override {
        // MCR p15, 0, Rt, c13, c0, 3 — write TPIDRURO
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 3) {
            return &tpidruro;
        }

        // MCR p15, 0, Rt, c1, c0, 0 — write SCTLR
        if (!two && CRn == CoprocReg::C1 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &sctlr;
        }

        // MCR p15, 0, Rt, c3, c0, 0 — write DACR
        if (!two && CRn == CoprocReg::C3 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &dacr;
        }

        LOG_WARN("Unhandled CP15 MCR: two={} opc1={} CRn={} CRm={} opc2={}", two, opc1, (int)CRn, (int)CRm, opc2);
        return CallbackOrAccessOneWord{};
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return CallbackOrAccessTwoWords{};
    }

    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        // MRC p15, 0, Rt, c13, c0, 3 — read TPIDRURO (thread-local storage)
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 3) {
            return &tpidruro;
        }

        // MRC p15, 0, Rt, c1, c0, 0 — read SCTLR
        if (!two && CRn == CoprocReg::C1 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &sctlr;
        }

        // MRC p15, 0, Rt, c3, c0, 0 — read DACR
        if (!two && CRn == CoprocReg::C3 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 0) {
            return &dacr;
        }

        LOG_WARN("Unhandled CP15 MRC: two={} opc1={} CRn={} CRm={} opc2={}", two, opc1, (int)CRn, (int)CRm, opc2);
        return CallbackOrAccessOneWord{};
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) override {
        return CallbackOrAccessTwoWords{};
    }

    std::optional<Callback> CompileLoadWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return std::nullopt;
    }

    std::optional<Callback> CompileStoreWords(bool two, bool long_transfer, CoprocReg CRd,
        std::optional<std::uint8_t> option) override {
        return std::nullopt;
    }

    void set_tpidruro(uint32_t tpidruro) {
        this->tpidruro = tpidruro;
    }

    uint32_t get_tpidruro() const {
        return tpidruro;
    }
};

class ArmDynarmicCallback : public Dynarmic::A32::UserCallbacks {
    friend class DynarmicCPU;

    CPUState *parent;
    DynarmicCPU *cpu;

public:
    explicit ArmDynarmicCallback(CPUState &parent, DynarmicCPU &cpu)
        : parent(&parent)
        , cpu(&cpu) {}

    ~ArmDynarmicCallback() override = default;

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr addr) override {
        if (cpu->log_mem)
            LOG_TRACE("Instruction fetch at address 0x{:X}", addr);
        return MemoryRead32(addr);
    }

    static void TraceInstruction(uint64_t self_, uint64_t address, uint64_t is_thumb) {
        ArmDynarmicCallback &self = *reinterpret_cast<ArmDynarmicCallback *>(self_);

        std::string disassembly = [&]() -> std::string {
            if (!address || !Ptr<uint32_t>{ (uint32_t)address }.valid(*self.parent->mem)) {
                return "invalid address";
            }
            return disassemble(*self.parent, address);
        }();
        LOG_TRACE("{} ({}): {} {}", log_hex(self_), self.parent->thread_id, log_hex(address), disassembly);
    }

    // Recompiled code (cpu/aot.h): a block that reaches the entry of a native function, or the
    // sentinel native code's calls into the JIT return to, ends there and stops the JIT with
    // UserDefined7; DynarmicCPU::aot_boundary takes over.
    static void AotBoundary(uint64_t cpu_) {
        reinterpret_cast<DynarmicCPU *>(cpu_)->jit->HaltExecution(Dynarmic::HaltReason::UserDefined7);
    }

    bool PreCodeReadHook(bool is_thumb, Dynarmic::A32::VAddr pc, Dynarmic::A32::IREmitter &ir) override {
        if (!aot_enabled())
            return true;
        const uint32_t entry = pc | (is_thumb ? 1u : 0u);
        if (pc != aot_sentinel() && !aot_lookup(entry) && !aot_lookup_resume(entry))
            return true;
        ir.BranchWritePC(ir.Imm32(pc));
        ir.CallHostFunction(&AotBoundary, ir.Imm64(reinterpret_cast<uint64_t>(cpu)));
        ir.SetTerm(Dynarmic::IR::Term::CheckHalt{ Dynarmic::IR::Term::ReturnToDispatch{} });
        return false;
    }

    void PreCodeTranslationHook(bool is_thumb, Dynarmic::A32::VAddr pc, Dynarmic::A32::IREmitter &ir) override {
        if (cpu->log_code) {
            ir.CallHostFunction(&TraceInstruction, ir.Imm64((uint64_t)this), ir.Imm64(pc), ir.Imm64(is_thumb));
        }
    }

    template <typename T>
    T MemoryRead(Dynarmic::A32::VAddr addr) {
        Ptr<T> ptr{ addr };
        if (!ptr || !ptr.valid(*parent->mem) || ptr.address() < parent->mem->host_page_size) {
            LOG_ERROR("Invalid read of uint{}_t at address: 0x{:x}\n{}", sizeof(T) * 8, addr, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < parent->mem->host_page_size)
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return 0;
        }

        T ret = *ptr.get(*parent->mem);
        if (cpu->log_mem) {
            LOG_TRACE("Read uint{}_t at address: 0x{:x}, val = 0x{:x}", sizeof(T) * 8, addr, ret);
        }
        return ret;
    }

    uint8_t MemoryRead8(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint8_t>(addr);
    }

    uint16_t MemoryRead16(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint16_t>(addr);
    }

    uint32_t MemoryRead32(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint32_t>(addr);
    }

    uint64_t MemoryRead64(Dynarmic::A32::VAddr addr) override {
        return MemoryRead<uint64_t>(addr);
    }

    template <typename T>
    void MemoryWrite(Dynarmic::A32::VAddr addr, T value) {
        Ptr<T> ptr{ addr };
        if (!ptr || !ptr.valid(*parent->mem) || ptr.address() < parent->mem->host_page_size) {
            LOG_ERROR("Invalid write of uint{}_t at addr: 0x{:x}, val = 0x{:x}\n{}", sizeof(T) * 8, addr, value, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < parent->mem->host_page_size)
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return;
        }

        *ptr.get(*parent->mem) = value;
        if (cpu->log_mem) {
            LOG_TRACE("Write uint{}_t at addr: 0x{:x}, val = 0x{:x}", sizeof(T) * 8, addr, value);
        }
    }

    void MemoryWrite8(Dynarmic::A32::VAddr addr, uint8_t value) override {
        MemoryWrite<uint8_t>(addr, value);
    }

    void MemoryWrite16(Dynarmic::A32::VAddr addr, uint16_t value) override {
        MemoryWrite<uint16_t>(addr, value);
    }

    void MemoryWrite32(Dynarmic::A32::VAddr addr, uint32_t value) override {
        MemoryWrite<uint32_t>(addr, value);
    }

    void MemoryWrite64(Dynarmic::A32::VAddr addr, uint64_t value) override {
        MemoryWrite<uint64_t>(addr, value);
    }

    template <typename T>
    bool MemoryWriteExclusive(Dynarmic::A32::VAddr addr, T value, T expected) {
        Ptr<T> ptr{ addr };
        if (!ptr || !ptr.valid(*parent->mem) || ptr.address() < parent->mem->host_page_size) {
            LOG_ERROR("Invalid exclusive write of uint{}_t at addr: 0x{:x}, val = 0x{:x}, expected = 0x{:x}\n{}", sizeof(T) * 8, addr, value, expected, this->cpu->save_context().description());

            auto pc = this->cpu->get_pc();
            if (pc < parent->mem->host_page_size)
                LOG_CRITICAL("PC is 0x{:x}", pc);
            else
                LOG_ERROR("Executing: {}", disassemble(*parent, pc, nullptr));
            return false;
        }

        auto result = Ptr<T>(addr).atomic_compare_and_swap(*parent->mem, value, expected);
        if (cpu->log_mem) {
            LOG_TRACE("Write uint{}_t at addr: 0x{:x}, val = 0x{:x}, expected = 0x{:x}", sizeof(T) * 8, addr, value, expected);
        }
        return result;
    }

    bool MemoryWriteExclusive8(Dynarmic::A32::VAddr addr, uint8_t value, uint8_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive16(Dynarmic::A32::VAddr addr, uint16_t value, uint16_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive32(Dynarmic::A32::VAddr addr, uint32_t value, uint32_t expected) override {
        return MemoryWriteExclusive(addr, value, expected);
    }

    bool MemoryWriteExclusive64(Dynarmic::A32::VAddr addr, uint64_t value, uint64_t expected) override {
        return MemoryWriteExclusive(addr, value, expected); // Ptr<uint64_t>(addr).atomic_compare_and_swap(*parent->mem, value, expected);
    }

    void InterpreterFallback(Dynarmic::A32::VAddr addr, size_t num_insts) override {
        LOG_ERROR("Unimplemented instruction at address {}:\n{}", log_hex(addr), save_context(*parent).description());
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::Breakpoint: {
            LOG_WARN("ss-delta: breakpoint instruction at {:08x} (thread {})", pc, parent->thread_id);
            cpu->break_ = true;
            cpu->jit->HaltExecution();
            if (cpu->is_thumb_mode())
                cpu->set_pc(pc | 1);
            else
                cpu->set_pc(pc);
            break;
        }
        case Dynarmic::A32::Exception::WaitForInterrupt: {
            cpu->halted = true;
            cpu->jit->HaltExecution();
            break;
        }
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadInstruction:
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForEvent:
            break;
        case Dynarmic::A32::Exception::Yield:
            break;
        case Dynarmic::A32::Exception::UndefinedInstruction:
            LOG_WARN("Undefined instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::UnpredictableInstruction:
            LOG_WARN("Unpredictable instruction at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        case Dynarmic::A32::Exception::DecodeError: {
            LOG_WARN("Decode error at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
            InterpreterFallback(pc, 1);
            break;
        }
        default:
            LOG_WARN("Unknown exception {} Raised at pc = 0x{:x}", static_cast<size_t>(exception), pc);
            LOG_TRACE("at address 0x{:X}, instruction 0x{:X} ({})", pc, MemoryReadCode(pc).value(), disassemble(*parent, pc, nullptr));
        }
    }

    void CallSVC(uint32_t svc) override {
        parent->svc_called = true;
        parent->svc = svc;
        cpu->jit->HaltExecution(Dynarmic::HaltReason::UserDefined8);
    }

    void AddTicks(uint64_t ticks) override {
        // only called with SSD_AOT_COUNT=1 (enable_cycle_counting)
        cpu->jit_ticks += ticks;
        aot_add_jit_insns(ticks);
    }

    uint64_t GetTicksRemaining() override {
        return 1ull << 60;
    }
};

Dynarmic::ExclusiveMonitor DynarmicCPU::shared_monitor(MAX_CORE_COUNT);

// Differential mode (SSD_AOT_DIFF): the shadow JIT runs a sampled native call first, from the same
// registers, with every memory write journaled. A call that makes an HLE call (an SVC), stores
// exclusively, touches invalid memory or runs too long is skipped; otherwise its results are recorded, its writes are
// undone, the native code runs for real and the two are compared: registers r0-r13, the VFP
// registers, FPSCR and every byte the shadow wrote. Writes only the native code makes are not
// seen.
struct AotShadowWrite {
    uint32_t addr;
    uint32_t size;
    uint64_t old;
    uint64_t final_value;
};

// One check's shadow results; checks nest (native code -> JIT -> another sampled native call), so
// each boundary keeps its own
struct AotShadowResult {
    std::array<uint32_t, 16> entry{};
    std::array<uint32_t, 16> regs{};
    std::array<uint32_t, 64> ext{};
    uint32_t fpscr = 0;
    std::vector<AotShadowWrite> journal;
};

struct AotShadow final : Dynarmic::A32::UserCallbacks {
    using Write = AotShadowWrite;

    CPUState *state = nullptr;
    // dynarmic needs a monitor to translate exclusives; the shadow's is private
    Dynarmic::ExclusiveMonitor monitor{ 1 };
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::vector<Write> journal;
    const char *abort_reason = nullptr;
    bool returned = false;
    uint64_t ticks_left = 0;

    template <typename T>
    bool valid(uint32_t a) {
        const Ptr<T> ptr{ a };
        return ptr && ptr.valid(*state->mem) && a >= state->mem->host_page_size;
    }
    template <typename T>
    T rd(uint32_t a) {
        if (!valid<T>(a)) {
            abort("invalid read");
            return 0;
        }
        T v;
        memcpy(&v, state->mem->memory.get() + a, sizeof(T));
        return v;
    }
    template <typename T>
    void wr(uint32_t a, T v) {
        if (!valid<T>(a)) {
            abort("invalid write");
            return;
        }
        uint64_t old = 0;
        memcpy(&old, state->mem->memory.get() + a, sizeof(T));
        journal.push_back({ a, sizeof(T), old, 0 });
        memcpy(state->mem->memory.get() + a, &v, sizeof(T));
    }
    void abort(const char *why) {
        if (!abort_reason)
            abort_reason = why;
        jit->HaltExecution();
    }

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr a) override {
        if (!valid<uint32_t>(a))
            return std::nullopt;
        return rd<uint32_t>(a);
    }
    uint8_t MemoryRead8(Dynarmic::A32::VAddr a) override { return rd<uint8_t>(a); }
    uint16_t MemoryRead16(Dynarmic::A32::VAddr a) override { return rd<uint16_t>(a); }
    uint32_t MemoryRead32(Dynarmic::A32::VAddr a) override { return rd<uint32_t>(a); }
    uint64_t MemoryRead64(Dynarmic::A32::VAddr a) override { return rd<uint64_t>(a); }
    void MemoryWrite8(Dynarmic::A32::VAddr a, uint8_t v) override { wr(a, v); }
    void MemoryWrite16(Dynarmic::A32::VAddr a, uint16_t v) override { wr(a, v); }
    void MemoryWrite32(Dynarmic::A32::VAddr a, uint32_t v) override { wr(a, v); }
    void MemoryWrite64(Dynarmic::A32::VAddr a, uint64_t v) override { wr(a, v); }
    // exclusives synchronise with other threads, which keep running between the shadow's run and
    // the native one (and would see the shadow's writes undone): such calls are not compared
    template <typename T>
    bool wr_excl(uint32_t, T, T) {
        abort("atomics");
        return false;
    }
    bool MemoryWriteExclusive8(Dynarmic::A32::VAddr a, uint8_t v, uint8_t e) override { return wr_excl(a, v, e); }
    bool MemoryWriteExclusive16(Dynarmic::A32::VAddr a, uint16_t v, uint16_t e) override { return wr_excl(a, v, e); }
    bool MemoryWriteExclusive32(Dynarmic::A32::VAddr a, uint32_t v, uint32_t e) override { return wr_excl(a, v, e); }
    bool MemoryWriteExclusive64(Dynarmic::A32::VAddr a, uint64_t v, uint64_t e) override { return wr_excl(a, v, e); }
    void InterpreterFallback(Dynarmic::A32::VAddr, size_t) override { abort("interpreter fallback"); }
    void CallSVC(uint32_t) override { abort("HLE call"); }
    void ExceptionRaised(uint32_t, Dynarmic::A32::Exception e) override {
        switch (e) {
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadInstruction:
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::Yield:
            break;
        default:
            abort("exception");
        }
    }
    void AddTicks(uint64_t t) override { ticks_left = t > ticks_left ? 0 : ticks_left - t; }
    uint64_t GetTicksRemaining() override { return ticks_left; }

    static void Returned(uint64_t self_) {
        AotShadow &self = *reinterpret_cast<AotShadow *>(self_);
        self.returned = true;
        self.jit->HaltExecution();
    }
    bool PreCodeReadHook(bool, Dynarmic::A32::VAddr pc, Dynarmic::A32::IREmitter &ir) override {
        if (pc != aot_sentinel())
            return true;
        ir.BranchWritePC(ir.Imm32(pc));
        ir.CallHostFunction(&Returned, ir.Imm64(reinterpret_cast<uint64_t>(this)));
        ir.SetTerm(Dynarmic::IR::Term::CheckHalt{ Dynarmic::IR::Term::ReturnToDispatch{} });
        return false;
    }
};

std::unique_ptr<Dynarmic::A32::Jit> DynarmicCPU::make_jit() {
    Dynarmic::A32::UserConfig config{};
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.callbacks = cb.get();
    if (parent->mem->use_page_table) {
        config.page_table = (log_mem || !cpu_opt) ? nullptr : reinterpret_cast<decltype(config.page_table)>(parent->mem->page_table.get());
        config.absolute_offset_page_table = true;
    } else if (!log_mem && cpu_opt) {
        config.fastmem_pointer = std::bit_cast<uintptr_t>(parent->mem->memory.get());
    }
    config.hook_hint_instructions = true;
    config.global_monitor = &shared_monitor;
    config.coprocessors[15] = cp15;
    config.processor_id = core_id;
    config.optimizations = cpu_opt ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
    config.enable_cycle_counting = aot_count_jit_insns();

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

DynarmicCPU::DynarmicCPU(CPUState *state, std::size_t processor_id, bool cpu_opt)
    : parent(state)
    , cb(std::make_unique<ArmDynarmicCallback>(*state, *this))
    , cp15(std::make_shared<ArmDynarmicCP15>())
    , core_id(processor_id)
    , cpu_opt(cpu_opt) {
    jit = make_jit();
}

DynarmicCPU::~DynarmicCPU() = default;

// Runs the call at pc on the shadow JIT from the JIT's current registers; true when it returned
// normally, with its results recorded and its writes undone
bool DynarmicCPU::aot_diff_shadow(uint32_t pc, AotShadowResult &out) {
    if (!shadow) {
        shadow = std::make_unique<AotShadow>();
        shadow->state = parent;
        Dynarmic::A32::UserConfig config{};
        config.arch_version = Dynarmic::A32::ArchVersion::v7;
        config.callbacks = shadow.get();
        config.coprocessors[15] = cp15;
        config.global_monitor = &shadow->monitor;
        config.processor_id = 0;
        config.optimizations = cpu_opt ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
        config.enable_cycle_counting = true;
        shadow->jit = std::make_unique<Dynarmic::A32::Jit>(config);
    }
    AotShadow &s = *shadow;
    out.entry = jit->Regs();
    s.jit->Regs() = jit->Regs();
    s.jit->Regs()[14] = aot_sentinel() | 1;
    s.jit->ExtRegs() = jit->ExtRegs();
    s.jit->SetCpsr(jit->Cpsr());
    s.jit->SetFpscr(jit->Fpscr());
    s.jit->ClearExclusiveState();
    s.monitor.ClearProcessor(0);
    // exclusive loads read what other threads change concurrently, like exclusive stores: any
    // exclusive access (which moves the reservation off this tag) skips the comparison
    constexpr Dynarmic::VAddr untouched = 0xa07a07a0'00000000ull;
    Dynarmic::VAddr &reservation = *Dynarmic::GetExclusiveMonitorAddressPointer(&s.monitor, 0);
    reservation = untouched;
    s.journal.clear();
    s.abort_reason = nullptr;
    s.returned = false;
    s.ticks_left = 20'000'000;
    const auto t0 = std::chrono::steady_clock::now();
    int spurious = 0;
    while (!s.returned && !s.abort_reason) {
        const Dynarmic::HaltReason hr = s.jit->Run();
        if (s.ticks_left == 0 && !s.returned)
            s.abort("too long");
        else if (hr != Dynarmic::HaltReason::Step && hr != Dynarmic::HaltReason::CacheInvalidation && !s.returned && !s.abort_reason
            && ++spurious > 2) {
            // a halt left over from an earlier aborted run returns once; anything more is unexpected
            static thread_local std::string why;
            why = fmt::format("stopped {:x} at {:08x}", static_cast<uint32_t>(hr), s.jit->Regs()[15]);
            s.abort(why.c_str());
        }
    }
    if (reservation != untouched)
        s.abort("atomics");
    s.monitor.ClearProcessor(0);
    aot_count_diff_cost(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(), 20'000'000 - s.ticks_left);
    uint8_t *const base = parent->mem->memory.get();
    if (!s.abort_reason) {
        out.regs = s.jit->Regs();
        out.ext = s.jit->ExtRegs();
        out.fpscr = s.jit->Fpscr();
        for (auto &w : s.journal)
            memcpy(&w.final_value, base + w.addr, w.size);
        out.journal = s.journal;
    } else {
        aot_count_diff(pc, false, false, s.abort_reason);
    }
    for (auto it = s.journal.rbegin(); it != s.journal.rend(); ++it)
        memcpy(base + it->addr, &it->old, it->size);
    return !s.abort_reason;
}

// Compares the native call that just returned with the shadow's run of it
void DynarmicCPU::aot_diff_compare(uint32_t pc, const AotShadowResult &s) {
    // native code's calls into the JIT and the shadow's own call return to the sentinel, so a
    // pushed return address can be the sentinel on one side and the real one on the other
    const uint32_t sentinel = aot_sentinel() | 1;
    const auto same = [sentinel](uint64_t a, uint64_t b, uint32_t size) {
        return a == b || (size == 4 && (a == sentinel || b == sentinel));
    };
    std::string diff;
    for (int i = 0; i < 14; i++) {
        if (!same(guest.r[i], s.regs[i], 4))
            diff += fmt::format(" r{} {:08x}/{:08x}", i, guest.r[i], s.regs[i]);
    }
    for (int i = 0; i < 64; i++) {
        if (guest.vfp.s[i] != s.ext[i]) {
            diff += fmt::format(" s{} {:08x}/{:08x}", i, guest.vfp.s[i], s.ext[i]);
            break;
        }
    }
    if ((guest.fpscr & 0xffffff00u) != (s.fpscr & 0xffffff00u))
        diff += fmt::format(" fpscr {:08x}/{:08x}", guest.fpscr, s.fpscr);
    const uint8_t *const base = parent->mem->memory.get();
    int memory_diffs = 0;
    for (const auto &w : s.journal) {
        uint64_t now = 0;
        memcpy(&now, base + w.addr, w.size);
        if (!same(now, w.final_value, w.size) && memory_diffs++ < 4)
            diff += fmt::format(" [{:08x}] {:x}/{:x}", w.addr, now, w.final_value);
    }
    if (memory_diffs > 4)
        diff += fmt::format(" ({} memory differences)", memory_diffs);
    aot_count_diff(pc, true, !diff.empty(), nullptr);
    if (!diff.empty()) {
        LOG_ERROR("AOT diff {:08x} (native/JIT):{}", pc, diff);
        aot_note_mismatch(fmt::format("{:08x} (native/JIT):{}; entry r0-3 {:08x} {:08x} {:08x} {:08x} sp {:08x} lr {:08x}; shadow pc {:08x} lr {:08x}",
            pc, diff, s.entry[0], s.entry[1], s.entry[2], s.entry[3], s.entry[13], s.entry[14], s.regs[15], s.regs[14]));
    }
}

namespace {

constexpr auto AOT_HALT = Dynarmic::HaltReason::UserDefined7;

AotOwner &aot_owner(armrecomp::GuestContext *ctx) {
    return *static_cast<AotOwner *>(ctx->embedder);
}

} // namespace

int DynarmicCPU::run() {
    halted = false;
    break_ = false;
    parent->svc_called = false;
    try {
        while (true) {
            const uint32_t start_pc = jit->Regs()[15];
            const uint64_t start_ticks = jit_ticks;
            const Dynarmic::HaltReason halt_reason = jit->Run();
            if (jit_ticks != start_ticks)
                aot_profile_jit(start_pc, jit_ticks - start_ticks, false);
            if (Dynarmic::Has(halt_reason, AOT_HALT)) {
                if (aot_boundary()) {
                    // native code's calls into the JIT return to the sentinel inside aot_call_guest
                    LOG_ERROR("AOT: guest code returned to the sentinel outside a call from native code");
                    return -1;
                }
                if (halt_reason == AOT_HALT)
                    continue;
                break; // also stopped for another reason: return to the run loop
            }
            if (halt_reason != Dynarmic::HaltReason::Step && halt_reason != Dynarmic::HaltReason::CacheInvalidation)
                break;
        }
    } catch (const AotUnwind &) {
        // the thread is exiting; its run loop sees why
    }

    return halted;
}

void DynarmicCPU::aot_sync_to_guest() {
    std::copy(jit->Regs().begin(), jit->Regs().end(), guest.r);
    const uint32_t cpsr = jit->Cpsr();
    guest.n = (cpsr >> 31) & 1;
    guest.z = (cpsr >> 30) & 1;
    guest.c = (cpsr >> 29) & 1;
    guest.v = (cpsr >> 28) & 1;
    guest.q = (cpsr >> 27) & 1;
    guest.ge = (cpsr >> 16) & 0xf;
    static_assert(sizeof(guest.vfp) == sizeof(jit->ExtRegs()));
    memcpy(guest.vfp.s, jit->ExtRegs().data(), sizeof(guest.vfp));
    guest.fpscr = jit->Fpscr();
    guest.tpidruro = cp15->get_tpidruro();
    guest.excl_valid = 0;
    guest.embedder = static_cast<AotOwner *>(this);
}

void DynarmicCPU::aot_sync_from_guest() {
    std::copy(guest.r, guest.r + 16, jit->Regs().begin());
    // mode, A/I/F from the JIT; IT state is clear at every boundary; set_pc sets T
    const uint32_t cpsr = (jit->Cpsr() & 0x1df) | guest.n << 31 | guest.z << 30 | guest.c << 29 | guest.v << 28 | guest.q << 27 | (guest.ge & 0xf) << 16;
    jit->SetCpsr(cpsr);
    memcpy(jit->ExtRegs().data(), guest.vfp.s, sizeof(guest.vfp));
    jit->SetFpscr(guest.fpscr);
    cp15->set_tpidruro(guest.tpidruro); // kernel modules write it (mcr)
}

// The JIT stopped at a native entry or at the sentinel (true). Runs the native function and puts
// the JIT where it returned or escaped to.
bool DynarmicCPU::aot_boundary() {
    const uint32_t pc = jit->Regs()[15];
    if (pc == aot_sentinel())
        return true;
    const uint32_t entry = pc | (is_thumb_mode() ? 1u : 0u);
    AotFn fn = aot_lookup(entry);
    // a resume point: a frame whose callee escaped returns here, and its function continues natively
    const bool resume = !fn;
    if (resume)
        fn = aot_lookup_resume(entry);
    if (!fn) {
        LOG_ERROR("AOT: the JIT stopped at {} without native code", log_hex(pc));
        return false;
    }
    aot_count_native_entry();
    if (resume)
        aot_count_resume_entry();
    // differential mode: the shadow JIT runs the call first (calls only: a resumed frame does not
    // return to the sentinel)
    const uint64_t diff_interval = aot_diff_interval();
    AotShadowResult shadow_result;
    const bool diff = !resume && diff_interval && ++diff_counter % diff_interval == 0 && aot_diff_shadow(pc, shadow_result);
    aot_sync_to_guest();
    if (resume)
        guest.resume = pc;
    // SSD_AOT_TRACE=N logs the first N native calls (debugging)
    static const int64_t trace_limit = [] {
        const char *const env = std::getenv("SSD_AOT_TRACE");
        return env ? std::strtoll(env, nullptr, 10) : 0;
    }();
    static std::atomic<int64_t> traced{ 0 };
    const bool trace = trace_limit && traced.fetch_add(1) < trace_limit;
    if (trace)
        LOG_INFO("AOT enter {:08x} r0-3 {:08x} {:08x} {:08x} {:08x} sp {:08x} lr {:08x}", pc, guest.r[0], guest.r[1], guest.r[2], guest.r[3], guest.r[13], guest.r[14]);
    uint32_t next;
    try {
        fn(&guest);
        next = guest.r[15];
        if (diff)
            aot_diff_compare(pc, shadow_result);
    } catch (const AotEscape &e) {
        next = e.pc;
        if (trace)
            LOG_INFO("AOT escape from {:08x} to {:08x}", pc, next);
    }
    if (trace)
        LOG_INFO("AOT leave {:08x} -> {:08x} r0-1 {:08x} {:08x} sp {:08x}", pc, next, guest.r[0], guest.r[1], guest.r[13]);
    aot_add_native_insns(guest.insns);
    guest.insns = 0;
    aot_sync_from_guest();
    set_pc(next);
    return false;
}

// Runs the JIT until guest code returns to the sentinel, handling what run() leaves to the
// thread's run loop: imports and native entries.
void DynarmicCPU::aot_run_until_sentinel(uint32_t target) {
    while (true) {
        parent->svc_called = false;
        halted = false;
        const uint64_t start_ticks = jit_ticks;
        const Dynarmic::HaltReason halt_reason = jit->Run();
        if (jit_ticks != start_ticks)
            aot_profile_jit(target, jit_ticks - start_ticks, true);
        const bool ended = halted;
        if (parent->svc_called)
            aot_import(*Ptr<uint32_t>(get_pc() + 4).get(*parent->mem));
        if (parent->aot_unwind)
            throw AotUnwind{};
        if (Dynarmic::Has(halt_reason, AOT_HALT) && aot_boundary())
            return;
        if (ended) {
            // the guest returned past the sentinel to the thread's halt instruction
            LOG_ERROR("AOT: guest code called from native code ended its thread at {}", log_hex(get_pc()));
            throw AotUnwind{};
        }
        // Step, CacheInvalidation, a stop() for a suspend: keep going
    }
}

// An HLE call made while this CPU is inside run() (the run loop makes them between runs). Guest
// callbacks the HLE function runs end on the halt instruction and set halted/break_ for their own
// run(), not for this one.
void DynarmicCPU::aot_import(uint32_t nid) {
    const bool was_halted = halted, was_break = break_;
    parent->import_handler(*parent, nid);
    clear_exclusive();
    halted = was_halted;
    break_ = was_break;
}

void DynarmicCPU::aot_call_guest(uint32_t target) {
    aot_count_guest_call();
    const uint32_t ret = guest.r[14];
    static const bool trace = std::getenv("SSD_AOT_TRACE") != nullptr;
    if (trace)
        LOG_INFO("AOT call JIT {:08x} r0-3 {:08x} {:08x} {:08x} {:08x} sp {:08x} ret {:08x}", target, guest.r[0], guest.r[1], guest.r[2], guest.r[3], guest.r[13], ret);
    aot_sync_from_guest();
    jit->Regs()[14] = aot_sentinel() | 1;
    set_pc(target);
    aot_run_until_sentinel(target);
    aot_sync_to_guest();
    guest.r[14] = ret;
    guest.r[15] = ret;
}

void DynarmicCPU::aot_call_import(uint32_t stub) {
    const uint32_t *const words = Ptr<uint32_t>(stub).get(*parent->mem);
    if (words[0] != 0xef000000 || words[1] != 0xe1a0f00e) {
        // bound to another module's export: movw r12, #lo; movt r12, #hi; bx r12. When the export
        // has native code, call it directly (as the stub would, with r12 = target)
        if ((words[0] & 0x0ff0f000) == 0x0300c000 && (words[1] & 0x0ff0f000) == 0x0340c000 && (words[2] & 0x0fffffff) == 0x012fff1c) {
            const uint32_t lo = ((words[0] >> 4) & 0xf000) | (words[0] & 0xfff);
            const uint32_t hi = ((words[1] >> 4) & 0xf000) | (words[1] & 0xfff);
            const uint32_t target = (hi << 16) | lo;
            if (const AotFn fn = aot_lookup(target)) {
                guest.r[12] = target;
                fn(&guest);
                return;
            }
        }
        aot_call_guest(stub);
        return;
    }
    // svc #0; mov pc, lr; .word nid: the HLE function, as the run loop would call it
    aot_count_import_call();
    const uint32_t ret = guest.r[14], sp = guest.r[13];
    aot_sync_from_guest();
    set_pc(stub + 4);
    aot_import(words[2]);
    aot_sync_to_guest();
    if (parent->aot_unwind)
        throw AotUnwind{};
    if (get_pc() != stub + 4 || is_thumb_mode() || guest.r[13] != sp || guest.r[14] != ret) {
        // the HLE function switched contexts (SceFiber): continue there on the JIT. The other
        // context may be suspended in the same import, so pc alone does not show the switch
        aot_count_escape(stub);
        // a context suspended in an import resumes at its stub's `mov pc, lr`: return directly,
        // to land on the caller's resume point
        if (!is_thumb_mode() && *Ptr<uint32_t>(get_pc()).get(*parent->mem) == 0xe1a0f00e)
            throw AotEscape{ guest.r[14] };
        throw AotEscape{ get_pc() | (is_thumb_mode() ? 1u : 0u) };
    }
    guest.r[15] = ret;
}

// Differential mode also samples calls from native code to native code. A module built with
// RECOMP_BISECT makes every direct call one, so the whole call tree gets checked, not only the
// functions the JIT enters.
void DynarmicCPU::aot_call_native(uint32_t addr, AotFn fn) {
    const uint64_t diff_interval = aot_diff_interval();
    if (!diff_interval || ++diff_counter % diff_interval != 0) {
        fn(&guest);
        return;
    }
    // the JIT is stopped inside a boundary, which resets it afterwards: start the shadow from
    // the native context
    aot_sync_from_guest();
    set_pc(addr);
    AotShadowResult shadow_result;
    const bool diff = aot_diff_shadow(addr & ~1u, shadow_result);
    fn(&guest);
    if (diff)
        aot_diff_compare(addr & ~1u, shadow_result);
}

namespace aot_hooks {

void call_indirect(armrecomp::GuestContext *ctx, uint32_t addr) {
    if (const AotFn fn = aot_lookup(addr)) {
        aot_owner(ctx).aot_call_native(addr, fn);
        return;
    }
    if (!(addr & 1)) {
        // ARM code without native code may be an import stub reached through a pointer (the
        // eboot's __tls_get_addr): the import path calls its HLE function or native export
        // directly, and runs anything else on the JIT as before
        aot_owner(ctx).aot_call_import(addr);
        return;
    }
    aot_owner(ctx).aot_call_guest(addr);
}

void call_import(armrecomp::GuestContext *ctx, uint32_t stub_addr) {
    aot_owner(ctx).aot_call_import(stub_addr);
}

void escape(armrecomp::GuestContext *, uint32_t pc) {
    aot_count_escape(pc);
    throw AotEscape{ pc };
}

void unsupported(armrecomp::GuestContext *, uint32_t pc) {
    LOG_ERROR("AOT: no native code for the instruction at {}", log_hex(pc));
    aot_count_escape(pc);
    throw AotEscape{ pc };
}

} // namespace aot_hooks

int DynarmicCPU::step() {
    parent->svc_called = false;
    jit->Step();
    return 0;
}

bool DynarmicCPU::hit_breakpoint() {
    return break_;
}

void DynarmicCPU::trigger_breakpoint() {
    break_ = true;
    stop();
}

void DynarmicCPU::set_log_code(bool log) {
    if (log_code == log)
        return;

    log_code = log;
    jit = make_jit();
}

void DynarmicCPU::set_log_mem(bool log) {
    if (log_mem == log)
        return;

    log_mem = log;
    jit = make_jit();
}

bool DynarmicCPU::get_log_code() {
    return log_code;
}

bool DynarmicCPU::get_log_mem() {
    return log_mem;
}

void DynarmicCPU::stop() {
    jit->HaltExecution();
}

uint32_t DynarmicCPU::get_reg(uint8_t idx) {
    return jit->Regs()[idx];
}

uint32_t DynarmicCPU::get_sp() {
    return jit->Regs()[13];
}

uint32_t DynarmicCPU::get_pc() {
    return jit->Regs()[15];
}

void DynarmicCPU::set_reg(uint8_t idx, uint32_t val) {
    jit->Regs()[idx] = val;
}

void DynarmicCPU::set_cpsr(uint32_t val) {
    jit->SetCpsr(val);
}

uint32_t DynarmicCPU::get_tpidruro() {
    return cp15->get_tpidruro();
}

void DynarmicCPU::set_tpidruro(uint32_t val) {
    cp15->set_tpidruro(val);
}

void DynarmicCPU::set_pc(uint32_t val) {
    if (val & 1) {
        set_cpsr(get_cpsr() | 0x20);
        val = val & 0xFFFFFFFE;
    } else {
        set_cpsr(get_cpsr() & 0xFFFFFFDF);
        val = val & 0xFFFFFFFC;
    }
    jit->Regs()[15] = val;
}

void DynarmicCPU::set_lr(uint32_t val) {
    jit->Regs()[14] = val;
}

void DynarmicCPU::set_sp(uint32_t val) {
    jit->Regs()[13] = val;
}

uint32_t DynarmicCPU::get_cpsr() {
    return jit->Cpsr();
}

uint32_t DynarmicCPU::get_fpscr() {
    return jit->Fpscr();
}

void DynarmicCPU::set_fpscr(uint32_t val) {
    jit->SetFpscr(val);
}

CPUContext DynarmicCPU::save_context() {
    CPUContext ctx;
    ctx.cpu_registers = jit->Regs();
    static_assert(sizeof(ctx.fpu_registers) == sizeof(jit->ExtRegs()));
    memcpy(ctx.fpu_registers.data(), jit->ExtRegs().data(), sizeof(ctx.fpu_registers));
    ctx.fpscr = jit->Fpscr();
    ctx.cpsr = jit->Cpsr();

    return ctx;
}

void DynarmicCPU::load_context(const CPUContext &ctx) {
    jit->Regs() = ctx.cpu_registers;
    static_assert(sizeof(ctx.fpu_registers) == sizeof(jit->ExtRegs()));
    memcpy(jit->ExtRegs().data(), ctx.fpu_registers.data(), sizeof(ctx.fpu_registers));
    jit->SetCpsr(ctx.cpsr);
    jit->SetFpscr(ctx.fpscr);
}

uint32_t DynarmicCPU::get_lr() {
    return jit->Regs()[14];
}

float DynarmicCPU::get_float_reg(uint8_t idx) {
    return std::bit_cast<float>(jit->ExtRegs()[idx]);
}

void DynarmicCPU::set_float_reg(uint8_t idx, float val) {
    jit->ExtRegs()[idx] = std::bit_cast<uint32_t>(val);
}

bool DynarmicCPU::is_thumb_mode() {
    return jit->Cpsr() & 0x20;
}

std::size_t DynarmicCPU::processor_id() const {
    return core_id;
}

void DynarmicCPU::invalidate_jit_cache(Address start, size_t length) {
    jit->InvalidateCacheRange(start, length);
}

void DynarmicCPU::clear_exclusive() {
    shared_monitor.ClearProcessor(core_id);
}
