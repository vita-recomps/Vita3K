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

// The hooks recompiled code calls (aot_abi.h AotHost), for whichever CPU it runs on (AotOwner)

#include <cpu/aot.h>
#include <cpu/impl/aot_hooks.h>
#include <cpu/impl/aot_owner.h>
#include <util/log.h>

namespace {

AotOwner &aot_owner(armrecomp::GuestContext *ctx) {
    return *static_cast<AotOwner *>(ctx->embedder);
}

} // namespace

namespace aot_hooks {

void call_indirect(armrecomp::GuestContext *ctx, uint32_t addr) {
    if (const AotFn fn = aot_lookup(addr)) {
        aot_owner(ctx).aot_call_native(addr, fn);
        return;
    }
    if (!(addr & 1)) {
        // ARM code without native code may be an import stub reached through a pointer (the
        // eboot's __tls_get_addr): the import path calls its HLE function or native export
        // directly, and anything else as aot_call_guest does
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
