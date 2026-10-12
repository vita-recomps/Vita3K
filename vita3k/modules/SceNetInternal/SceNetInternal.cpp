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

#include <module/module.h>

#include "../SceNet/SceNet.h"

#include <mutex>
#include <random>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceNetInternal);

EXPORT(int, sceNetInternalInetPton, int af, const char *src, void *dst) {
    return CALL_EXPORT(sceNetInetPton, af, src, dst);
}

// ss-delta: called by the firmware's ad hoc matching module (adhoc_matching.suprx) with a pointer
// to one word, which it then uses as a nonzero seed (taken modulo a range; 0 is replaced by 1).
// Hypothesis: a random number, as from sceKernelGetRandomNumber.
EXPORT(int, SceNetInternal_689B9D7D, uint32_t *out) {
    TRACY_FUNC(SceNetInternal_689B9D7D, out);
    static std::mt19937 generator{ std::random_device{}() };
    static std::mutex mutex;
    if (!out)
        return 0;
    const std::lock_guard<std::mutex> lock(mutex);
    *out = generator();
    return 0;
}

EXPORT(int, sceNetInternalIcmConnect, int sid, int flags) {
    TRACY_FUNC(sceNetInternalIcmConnect, sid, flags);
    // call sceNetSyscallIcmConnect(sid, flags)
    return UNIMPLEMENTED();
}
