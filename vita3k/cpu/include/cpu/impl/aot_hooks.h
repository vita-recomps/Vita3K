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

// The hooks recompiled code calls (cpu/aot_abi.h AotHost), implemented next to the JIT in
// dynarmic_cpu.cpp.
#pragma once

#include <cpu/aot_abi.h>

namespace aot_hooks {
void call_indirect(armrecomp::GuestContext *ctx, uint32_t addr);
void call_import(armrecomp::GuestContext *ctx, uint32_t stub_addr);
void escape(armrecomp::GuestContext *ctx, uint32_t pc);
void unsupported(armrecomp::GuestContext *ctx, uint32_t pc);
} // namespace aot_hooks
