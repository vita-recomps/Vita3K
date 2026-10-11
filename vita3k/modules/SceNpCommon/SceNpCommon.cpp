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

#include <io/state.h>
#include <kernel/state.h>
#include <np/common.h>
#include <np/state.h>

#include <cstring>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceNpCommon);

enum SceNpUtilErrorCode {
    SCE_NP_UTIL_Ok = 0x0,
    SCE_NP_UTIL_ERROR_INVALID_ARGUMENT = 0x80550601,
    SCE_NP_UTIL_ERROR_INVALID_NP_ID = 0x80550605,
    SCE_NP_UTIL_ERROR_NOT_MATCH = 0x80550609,
};

EXPORT(int, sceNpAuthAbortRequest) {
    TRACY_FUNC(sceNpAuthAbortRequest);
    if (emuenv.np.entitlements.enabled)
        return 0;
    return UNIMPLEMENTED();
}

struct SceNpAuthRequestParameter {
    SceSize size;
    uint32_t version;
    Ptr<const char> serviceId;
    Ptr<const void> cookie;
    SceSize cookieSize;
    Ptr<const char> entitlementId;
    SceUInt32 consumedCount;
    Ptr<void> ticketCb; // int (*ticketCb)(SceNpAuthRequestId, int, void *);
    Ptr<void> cbArg;
};

struct SceNpEntitlementId {
    char data[32];
};

struct SceNpEntitlement {
    SceNpEntitlementId id;
    SceUInt64 createdDate;
    SceUInt64 expireDate;
    SceUInt32 type;
    SceInt32 remainingCount;
    SceInt32 consumedCount;
    char padding[4];
};
static_assert(sizeof(SceNpEntitlement) == 0x40);

// the placeholder ticket of offline entitlements: only its size matters to the game
constexpr SceSize OFFLINE_TICKET_SIZE = 256;
// any negative result means "not found" to the game (hypothesis: the exact code is unchecked)
constexpr int OFFLINE_ENTITLEMENT_NOT_FOUND = static_cast<int>(0x80550511);

EXPORT(int, sceNpAuthCreateStartRequest, const SceNpAuthRequestParameter *param) {
    TRACY_FUNC(sceNpAuthCreateStartRequest, param);
    auto &ent = emuenv.np.entitlements;
    if (ent.enabled) {
        // the game stores the request id before its callback may run: deliver it from
        // sceNpCheckCallback, as the system does
        std::lock_guard<std::mutex> lock(ent.mutex);
        const int id = ent.next_request_id++;
        LOG_INFO("NP auth (offline entitlements): request {} started", id);
        ent.pending.push_back({ id, param->ticketCb.address(), param->cbArg.address() });
        return id;
    }
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    // todo: this callback function should be called from sceNpCheckCallback
    STUBBED("Immediately call ticket callback");
    thread->run_callback(param->ticketCb.address(), { 1, 1, param->cbArg.address() });
    return 1;
}

EXPORT(int, sceNpAuthDestroyRequest) {
    TRACY_FUNC(sceNpAuthDestroyRequest);
    if (emuenv.np.entitlements.enabled)
        return 0;
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthGetEntitlementById, const void *ticket, SceSize ticket_size, const char *ent_id, SceNpEntitlement *ent) {
    TRACY_FUNC(sceNpAuthGetEntitlementById, ticket, ticket_size, ent_id, ent);
    auto &state = emuenv.np.entitlements;
    if (!state.enabled)
        return UNIMPLEMENTED();
    if (!ent_id || !ent)
        return OFFLINE_ENTITLEMENT_NOT_FOUND;
    const std::string wanted(ent_id, strnlen(ent_id, sizeof(SceNpEntitlementId::data)));
    LOG_INFO("NP auth (offline entitlements): entitlement {} asked", wanted);
    for (const auto &id : state.ids) {
        if (id != wanted)
            continue;
        *ent = {};
        std::strncpy(ent->id.data, id.c_str(), sizeof(ent->id.data) - 1);
        return 0;
    }
    return OFFLINE_ENTITLEMENT_NOT_FOUND;
}

EXPORT(int, sceNpAuthGetEntitlementByIdPrefix) {
    TRACY_FUNC(sceNpAuthGetEntitlementByIdPrefix);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthGetEntitlementIdList, const void *ticket, SceSize ticket_size, SceNpEntitlementId *list, SceSize count) {
    TRACY_FUNC(sceNpAuthGetEntitlementIdList, ticket, ticket_size, list, count);
    auto &state = emuenv.np.entitlements;
    if (!state.enabled)
        return UNIMPLEMENTED();
    LOG_INFO("NP auth (offline entitlements): entitlement list asked ({} slots), {} entitlements", count, state.ids.size());
    if (list) {
        for (SceSize i = 0; i < count && i < state.ids.size(); i++) {
            list[i] = {};
            std::strncpy(list[i].data, state.ids[i].c_str(), sizeof(list[i].data) - 1);
        }
    }
    return static_cast<int>(state.ids.size());
}

EXPORT(int, sceNpAuthGetTicket, int request_id, void *buf, SceSize size) {
    TRACY_FUNC(sceNpAuthGetTicket, request_id, buf, size);
    if (!emuenv.np.entitlements.enabled)
        return UNIMPLEMENTED();
    if (buf)
        std::memset(buf, 0, size);
    return 0;
}

EXPORT(int, sceNpAuthGetTicketParam) {
    TRACY_FUNC(sceNpAuthGetTicketParam);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthInit) {
    TRACY_FUNC(sceNpAuthInit);
    if (emuenv.np.entitlements.enabled) {
        LOG_INFO("NP auth (offline entitlements): init");
        return 0;
    }
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthTerm) {
    TRACY_FUNC(sceNpAuthTerm);
    if (emuenv.np.entitlements.enabled)
        return 0;
    return UNIMPLEMENTED();
}

// the ticket callbacks of offline entitlements, due from sceNpCheckCallback (SceNpManager)
void run_offline_ticket_callbacks(EmuEnvState &emuenv, SceUID thread_id) {
    auto &state = emuenv.np.entitlements;
    std::vector<NpOfflineEntitlements::PendingTicket> due;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        due.swap(state.pending);
    }
    if (due.empty())
        return;
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    for (const auto &t : due) {
        LOG_INFO("NP auth (offline entitlements): ticket for request {}, {} entitlements", t.request_id, state.ids.size());
        thread->run_callback(t.callback, { static_cast<uint32_t>(t.request_id), OFFLINE_TICKET_SIZE, t.arg });
    }
}

EXPORT(int, sceNpCmpNpId, np::SceNpId *npid1, np::SceNpId *npid2) {
    TRACY_FUNC(sceNpCmpNpId, npid1, npid2);

    if (npid1 == nullptr || npid2 == nullptr)
        return SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;
    if (!npid1->isIdValid || !npid2->isIdValid)
        return SCE_NP_UTIL_ERROR_INVALID_NP_ID;
    if (std::strncmp(npid1->handle.data, npid2->handle.data, SCE_NP_ONLINEID_MAX_LENGTH) != 0)
        return SCE_NP_UTIL_ERROR_NOT_MATCH;

    if (std::memcmp(npid1->opt.unknown, npid2->opt.unknown, 4) != 0)
        return SCE_NP_UTIL_ERROR_NOT_MATCH;

    if (npid1->opt.platformType[0] == 0 && npid2->opt.platformType[0] == 0)
        return SCE_NP_UTIL_Ok;
    if (std::memcmp(&npid1->opt.platformType, &npid2->opt.platformType, 4) != 0)
        return SCE_NP_UTIL_ERROR_NOT_MATCH;

    return SCE_NP_UTIL_Ok;
}

EXPORT(int, sceNpCmpNpIdInOrder) {
    TRACY_FUNC(sceNpCmpNpIdInOrder);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCmpOnlineId) {
    TRACY_FUNC(sceNpCmpOnlineId);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCommonBase64Encode) {
    TRACY_FUNC(sceNpCommonBase64Encode);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCommonFreeNpServerName) {
    TRACY_FUNC(sceNpCommonFreeNpServerName);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCommonGetNpEnviroment) {
    TRACY_FUNC(sceNpCommonGetNpEnviroment);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCommonGetSystemSwVersion) {
    TRACY_FUNC(sceNpCommonGetSystemSwVersion);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCommonMallocNpServerName) {
    TRACY_FUNC(sceNpCommonMallocNpServerName);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpGetPlatformType) {
    TRACY_FUNC(sceNpGetPlatformType);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSetPlatformType) {
    TRACY_FUNC(sceNpSetPlatformType);
    return UNIMPLEMENTED();
}
