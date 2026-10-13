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

#include <cpu/aot.h>
#include <cpu/aot_abi.h>
#include <cpu/impl/aot_hooks.h>

#include <mem/functions.h>
#include <mem/state.h>
#include <util/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

enum class Verified : int {
    unknown,
    yes,
    no,
};

struct LoadedModule {
    std::string path;
    armrecomp::AotInitFn init = nullptr;
    const armrecomp::AotModule *module = nullptr;
    uint32_t lo = 0, hi = 0; // [first entry, last entry]
    uint32_t end = 0; // end of the last function's code
    Address reserved = 0; // the code pages, held until the module loads (aot_release_reservation)
    std::atomic<Verified> verified{ Verified::unknown };
    std::mutex verify_mutex;
};

struct Registry {
    std::vector<std::unique_ptr<LoadedModule>> modules;
    // entry address (bit 0 = Thumb) -> function and its module; immutable after loading
    std::unordered_map<uint32_t, std::pair<AotFn, LoadedModule *>> functions;
    // resume point (bit 0 = Thumb) -> its function and module (aot_abi.h AotResume)
    std::unordered_map<uint32_t, std::pair<AotFn, LoadedModule *>> resumes;
    MemState *mem = nullptr;
    Address sentinel = 0;
    Address halt = 0;
    bool enabled = false;
    bool count_jit_insns = false;
};

Registry registry;

std::atomic<uint64_t> native_entries{ 0 }, resume_entries{ 0 }, guest_calls{ 0 }, import_calls{ 0 }, escapes{ 0 };
std::atomic<uint64_t> native_insns{ 0 }, jit_insns{ 0 };
std::atomic<int64_t> next_log{ 0 };
std::mutex escape_mutex;
std::map<uint32_t, uint64_t> escape_sites;
std::atomic<uint64_t> diff_checked{ 0 }, diff_skipped{ 0 }, diff_mismatches{ 0 }, diff_us{ 0 }, diff_insns{ 0 };
std::mutex diff_mutex;
std::map<uint32_t, uint64_t> diff_bad; // function -> mismatching calls
std::map<std::string, uint64_t> diff_skip_reasons;
std::vector<std::string> diff_recent; // last mismatch descriptions
std::mutex profile_mutex;
std::unordered_map<uint32_t, uint64_t> profile_called, profile_top; // key -> JIT instructions

// SSD_AOT_STATS=<file>: the counters, rewritten every 5 s with the native share of the last
// interval (the log is asynchronous and loses its tail when the process is killed)
void stats_thread(std::string path) {
    uint64_t last_native = 0, last_jit = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const uint64_t native = native_insns.load(), jit = jit_insns.load();
        const uint64_t dn = native - last_native, dj = jit - last_jit;
        last_native = native;
        last_jit = jit;
        if (FILE *f = std::fopen(path.c_str(), "w")) {
            std::fputs(fmt::format("last 5 s: native {} JIT {} ({:.1f}% native)\n{}\n", dn, dj,
                           dn + dj ? 100.0 * double(dn) / double(dn + dj) : 0.0, aot_counters_text())
                           .c_str(),
                f);
            std::fclose(f);
        }
    }
}

void load_modules() {
    const char *const count = std::getenv("SSD_AOT_COUNT");
    registry.count_jit_insns = count && *count == '1';
    if (const char *const stats = std::getenv("SSD_AOT_STATS"); stats && *stats)
        std::thread(stats_thread, std::string(stats)).detach();
#ifdef _WIN32
    const char *const env = std::getenv("SSD_AOT");
    if (!env || !*env)
        return;
    std::stringstream list{ std::string(env) };
    std::string path;
    while (std::getline(list, path, ';')) {
        if (path.empty())
            continue;
        const HMODULE dll = LoadLibraryA(path.c_str());
        if (!dll) {
            LOG_ERROR("AOT: cannot load {} (error {})", path, GetLastError());
            continue;
        }
        const auto init = reinterpret_cast<armrecomp::AotInitFn>(GetProcAddress(dll, ARMRECOMP_AOT_INIT));
        if (!init) {
            LOG_ERROR("AOT: {} has no {}", path, ARMRECOMP_AOT_INIT);
            continue;
        }
        auto m = std::make_unique<LoadedModule>();
        m->path = path;
        m->init = init;
        registry.modules.push_back(std::move(m));
    }
#else
    if (std::getenv("SSD_AOT"))
        LOG_ERROR("AOT: recompiled modules are only supported on Windows");
#endif
}

// FNV-1a 64 over every function's bytes in guest memory, in table order (aot_abi.h)
bool verify(LoadedModule &m) {
    const armrecomp::AotModule &mod = *m.module;
    const uint8_t *const base = registry.mem->memory.get();
    // the words the loader patched (variable imports) hash as zero
    const uint32_t *patched = mod.patched;
    const uint32_t *const patched_end = mod.patched + mod.patched_count;
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < mod.function_count; i++) {
        const uint32_t start = mod.functions[i].addr & ~1u;
        for (uint32_t k = 0; k < mod.functions[i].size; k++) {
            const uint32_t a = start + k;
            while (patched != patched_end && *patched + 4 <= a)
                patched++;
            const uint8_t b = (patched != patched_end && *patched <= a) ? 0 : base[a];
            hash = (hash ^ b) * 0x100000001b3ull;
        }
    }
    if (const char *const dump = std::getenv("SSD_AOT_DUMP"); dump && *dump && mod.function_count) {
        // the module's code as the runtime sees it, for comparison with the ELF
        const uint32_t last = mod.function_count - 1;
        const uint32_t start = mod.functions[0].addr & ~1u;
        const uint32_t end = (mod.functions[last].addr & ~1u) + mod.functions[last].size;
        if (FILE *f = std::fopen(dump, "wb")) {
            std::fwrite(base + start, 1, end - start, f);
            std::fclose(f);
            LOG_INFO("AOT: wrote {:08x}-{:08x} to {}", start, end, dump);
        }
    }
    if (hash != mod.code_hash) {
        LOG_ERROR("AOT: {} does not match the code in guest memory (hash {:016x}, expected {:016x}); running it on the JIT",
            mod.name, hash, mod.code_hash);
        return false;
    }
    LOG_INFO("AOT: {} matches guest memory, native code enabled", mod.name);
    return true;
}

} // namespace

void aot_init(MemState &mem, Address sentinel) {
    static std::once_flag loaded;
    std::call_once(loaded, load_modules);
    registry.enabled = false;
    registry.functions.clear();
    registry.resumes.clear();
    if (registry.modules.empty())
        return;
    if (mem.use_page_table) {
        LOG_ERROR("AOT: recompiled code needs the flat guest memory mapping; disabled");
        return;
    }
    registry.mem = &mem;
    registry.sentinel = sentinel;

    const armrecomp::AotHost host{
        armrecomp::AOT_ABI_VERSION,
        sizeof(armrecomp::GuestContext),
        mem.memory.get(),
        &aot_hooks::call_indirect,
        &aot_hooks::call_import,
        &aot_hooks::escape,
        &aot_hooks::unsupported,
    };
    for (auto &m : registry.modules) {
        m->module = m->init(&host);
        m->verified = Verified::unknown;
        if (!m->module) {
            LOG_ERROR("AOT: {} was built for another ABI version (runtime {})", m->path, armrecomp::AOT_ABI_VERSION);
            continue;
        }
        const armrecomp::AotModule &mod = *m->module;
        // SSD_AOT_RANGE=lo-hi[,lo-hi...] (hex, end exclusive): only these functions run natively,
        // for bisecting a misbehaving function
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        if (const char *const env = std::getenv("SSD_AOT_RANGE"); env && *env) {
            std::stringstream list{ std::string(env) };
            std::string item;
            while (std::getline(list, item, ',')) {
                const auto dash = item.find('-');
                if (dash != std::string::npos)
                    ranges.emplace_back(std::stoul(item.substr(0, dash), nullptr, 16), std::stoul(item.substr(dash + 1), nullptr, 16));
            }
        }
        uint32_t registered = 0;
        for (uint32_t i = 0; i < mod.function_count; i++) {
            const uint32_t a = mod.functions[i].addr & ~1u;
            if (!ranges.empty() && std::none_of(ranges.begin(), ranges.end(), [a](const auto &r) { return r.first <= a && a < r.second; }))
                continue;
            registry.functions.emplace(mod.functions[i].addr, std::make_pair(mod.functions[i].fn, m.get()));
            registered++;
        }
        // SSD_AOT_RESUME=0: frames the JIT took over after an escape stay on the JIT
        static const bool use_resumes = [] {
            const char *const env = std::getenv("SSD_AOT_RESUME");
            return !env || std::string(env) != "0";
        }();
        for (uint32_t i = 0; use_resumes && i < mod.resume_count; i++) {
            const armrecomp::AotResume &r = mod.resumes[i];
            const uint32_t a = r.function & ~1u;
            if (!ranges.empty() && std::none_of(ranges.begin(), ranges.end(), [a](const auto &rg) { return rg.first <= a && a < rg.second; }))
                continue;
            registry.resumes.emplace(r.addr, std::make_pair(r.fn, m.get()));
        }
        if (!ranges.empty())
            LOG_INFO("AOT: SSD_AOT_RANGE leaves {} of {} functions native", registered, mod.function_count);
        m->lo = mod.function_count ? mod.functions[0].addr & ~1u : 0;
        m->hi = mod.function_count ? mod.functions[mod.function_count - 1].addr & ~1u : 0;
        m->end = mod.function_count ? m->hi + mod.functions[mod.function_count - 1].size : 0;
        LOG_INFO("AOT: {} ({} functions, {:08x}-{:08x}) from {}", mod.name, mod.function_count, m->lo, m->hi, m->path);
    }

    // Hold each module's place so that nothing else is allocated there before it loads: a module
    // loaded late (ad hoc play) otherwise finds it taken, depending on what the session did
    // before, and its native code can't run. The segment is longer than the code (data after it),
    // so the hold runs up to the next module's code, at most 256 KB past this one's.
    std::vector<LoadedModule *> placed;
    for (auto &m : registry.modules)
        if (m->module && m->module->function_count)
            placed.push_back(m.get());
    std::sort(placed.begin(), placed.end(), [](const LoadedModule *a, const LoadedModule *b) { return a->lo < b->lo; });
    for (size_t i = 0; i < placed.size(); i++) {
        LoadedModule &m = *placed[i];
        const Address page = m.lo & ~0xFFFu;
        const Address code_end = (m.end + 0xFFFu) & ~0xFFFu;
        Address hold_end = code_end + 0x40000;
        if (i + 1 < placed.size())
            hold_end = std::max(code_end, std::min(hold_end, placed[i + 1]->lo & ~0xFFFu));
        const std::string name = fmt::format("aot_reserve:{}", m.module->name);
        m.reserved = try_alloc_at(mem, page, hold_end - page, name.c_str());
        if (!m.reserved)
            m.reserved = try_alloc_at(mem, page, code_end - page, name.c_str());
        LOG_WARN_IF(!m.reserved, "AOT: {:08x}-{:08x} is already taken; {} may not load where its native code expects it", page, code_end, m.module->name);
    }
    registry.enabled = !registry.functions.empty();
}

void aot_deinit() {
    if (registry.enabled)
        LOG_INFO("AOT: {}", aot_counters_text());
    registry.enabled = false;
    registry.functions.clear();
    registry.resumes.clear();
    registry.mem = nullptr;
    registry.sentinel = 0;
}

bool aot_enabled() {
    return registry.enabled;
}

Address aot_sentinel() {
    return registry.sentinel;
}

void aot_set_halt(Address halt) {
    registry.halt = halt;
}

Address aot_halt() {
    return registry.halt;
}

bool aot_native_cpu() {
    static const bool requested = [] {
        const char *const env = std::getenv("SSD_AOT_NATIVE");
        return env && std::string(env) == "1";
    }();
    return requested && registry.enabled;
}

namespace {

// native code once its module matches guest memory
AotFn checked(const std::pair<AotFn, LoadedModule *> &entry) {
    LoadedModule &m = *entry.second;
    Verified v = m.verified.load(std::memory_order_acquire);
    if (v == Verified::unknown) {
        const std::lock_guard<std::mutex> lock(m.verify_mutex);
        v = m.verified.load();
        if (v == Verified::unknown) {
            v = verify(m) ? Verified::yes : Verified::no;
            m.verified = v;
        }
    }
    return v == Verified::yes ? entry.first : nullptr;
}

} // namespace

void aot_release_reservation(MemState &mem, const std::string &module_stem) {
    for (const auto &m : registry.modules) {
        if (!m->reserved || !m->module || !m->module->name)
            continue;
        std::string name = m->module->name;
        if (const auto dot = name.rfind('.'); dot != std::string::npos)
            name.resize(dot);
        if (name == module_stem) {
            free(mem, m->reserved);
            m->reserved = 0;
        }
    }
}

Address aot_preferred_base(const std::string &module_stem) {
    for (const auto &m : registry.modules) {
        if (!m->module || !m->module->name || m->module->function_count == 0)
            continue;
        std::string name = m->module->name;
        if (const auto dot = name.rfind('.'); dot != std::string::npos)
            name.resize(dot);
        if (name == module_stem)
            return (m->module->functions[0].addr & ~1u) & ~0xFFFu;
    }
    return 0;
}

AotFn aot_lookup(uint32_t addr) {
    const auto it = registry.functions.find(addr);
    return it == registry.functions.end() ? nullptr : checked(it->second);
}

AotFn aot_lookup_resume(uint32_t addr) {
    const auto it = registry.resumes.find(addr);
    return it == registry.resumes.end() ? nullptr : checked(it->second);
}

void aot_count_resume_entry() {
    resume_entries.fetch_add(1, std::memory_order_relaxed);
}

bool aot_count_jit_insns() {
    return registry.count_jit_insns;
}

void aot_add_jit_insns(uint64_t n) {
    jit_insns.fetch_add(n, std::memory_order_relaxed);
}

void aot_add_native_insns(uint64_t n) {
    native_insns.fetch_add(n, std::memory_order_relaxed);
}

void aot_count_native_entry() {
    const uint64_t n = native_entries.fetch_add(1, std::memory_order_relaxed);
    if ((n & 0xffff) != 0)
        return;
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t due = next_log.load();
    if (now >= due && next_log.compare_exchange_strong(due, now + 10))
        LOG_INFO("AOT: {}", aot_counters_text());
}

void aot_profile_jit(uint32_t key, uint64_t insns, bool called) {
    const std::lock_guard<std::mutex> lock(profile_mutex);
    (called ? profile_called : profile_top)[key] += insns;
}

uint64_t aot_diff_interval() {
    static const uint64_t interval = [] {
        const char *const env = std::getenv("SSD_AOT_DIFF");
        return env ? std::strtoull(env, nullptr, 10) : 0;
    }();
    return interval;
}

void aot_note_mismatch(const std::string &text) {
    const std::lock_guard<std::mutex> lock(diff_mutex);
    if (diff_recent.size() == 12)
        diff_recent.erase(diff_recent.begin());
    diff_recent.push_back(text);
}

void aot_count_diff_cost(uint64_t microseconds, uint64_t insns) {
    diff_us.fetch_add(microseconds, std::memory_order_relaxed);
    diff_insns.fetch_add(insns, std::memory_order_relaxed);
}

void aot_count_diff(uint32_t pc, bool checked, bool mismatch, const char *skip_reason) {
    if (!checked) {
        diff_skipped.fetch_add(1, std::memory_order_relaxed);
        const std::lock_guard<std::mutex> lock(diff_mutex);
        diff_skip_reasons[skip_reason]++;
        return;
    }
    diff_checked.fetch_add(1, std::memory_order_relaxed);
    if (mismatch) {
        diff_mismatches.fetch_add(1, std::memory_order_relaxed);
        const std::lock_guard<std::mutex> lock(diff_mutex);
        diff_bad[pc]++;
    }
}

void aot_count_guest_call() {
    guest_calls.fetch_add(1, std::memory_order_relaxed);
}

void aot_count_import_call() {
    import_calls.fetch_add(1, std::memory_order_relaxed);
}

void aot_count_escape(uint32_t pc) {
    escapes.fetch_add(1, std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock(escape_mutex);
    escape_sites[pc]++;
}

std::string aot_counters_text() {
    std::string out = fmt::format("native entries {} (resumed {}), native->JIT calls {}, native->HLE calls {}, escapes {}",
        native_entries.load(), resume_entries.load(), guest_calls.load(), import_calls.load(), escapes.load());
    const uint64_t native = native_insns.load(), jit = jit_insns.load();
    if (registry.count_jit_insns && native + jit)
        out += fmt::format("; instructions native {} JIT {} ({:.1f}% native)", native, jit, 100.0 * double(native) / double(native + jit));
    else if (native)
        out += fmt::format("; native instructions {}", native);
    const std::lock_guard<std::mutex> lock(escape_mutex);
    std::vector<std::pair<uint64_t, uint32_t>> top;
    for (const auto &[pc, n] : escape_sites)
        top.emplace_back(n, pc);
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); i++)
        out += fmt::format("{}{:08x} x{}", i ? ", " : "; top escapes: ", top[i].second, top[i].first);
    if (aot_diff_interval()) {
        out += fmt::format("; differential: {} calls checked, {} mismatching, {} skipped; shadow runs took {} ms for {} instructions",
            diff_checked.load(), diff_mismatches.load(), diff_skipped.load(), diff_us.load() / 1000, diff_insns.load());
        const std::lock_guard<std::mutex> dlock(diff_mutex);
        for (const auto &[why, n] : diff_skip_reasons)
            out += fmt::format(" [{} {}]", why, n);
        size_t i = 0;
        for (const auto &[pc, n] : diff_bad) {
            if (i++ == 16)
                break;
            out += fmt::format("{}{:08x} x{}", i == 1 ? "; mismatching functions: " : ", ", pc, n);
        }
        for (const auto &m : diff_recent)
            out += "\n  mismatch " + m;
    }
    const std::lock_guard<std::mutex> plock(profile_mutex);
    for (const auto *map : { &profile_called, &profile_top }) {
        std::vector<std::pair<uint64_t, uint32_t>> heavy;
        for (const auto &[key, n] : *map)
            heavy.emplace_back(n, key);
        std::sort(heavy.rbegin(), heavy.rend());
        for (size_t i = 0; i < std::min<size_t>(heavy.size(), 12); i++)
            out += fmt::format("{}{:08x} {}M", i ? ", " : (map == &profile_called ? "; JIT called from native: " : "; JIT runs from: "), heavy[i].second, heavy[i].first / 1000000);
    }
    // JIT runs that start in a loaded module's code: the code the module does not cover natively
    std::map<uint32_t, uint64_t> inside;
    for (const auto *map : { &profile_called, &profile_top })
        for (const auto &[key, n] : *map)
            for (const auto &m : registry.modules)
                if (m->module && m->lo <= key && key < m->end)
                    inside[key] += n;
    if (registry.count_jit_insns) {
        uint64_t total = 0;
        for (const auto &[key, n] : inside)
            total += n;
        out += fmt::format("; JIT inside native modules: {} instructions from {} addresses", total, inside.size());
        size_t i = 0;
        for (const auto &[key, n] : inside) {
            if (i++ == 16)
                break;
            out += fmt::format("{}{:08x} {}", i == 1 ? " (" : ", ", key, n);
        }
        if (!inside.empty())
            out += ")";
    }
    return out;
}
