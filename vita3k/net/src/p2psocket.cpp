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

#include <net/socket.h>
#include <util/net_utils.h>

#include <bit>

#ifdef _WIN32
// mstcpip.h's SIO_UDP_CONNRESET (that header clashes with net/types.h)
constexpr DWORD SIO_UDP_CONNRESET_ = 0x9800000C; // _WSAIOW(IOC_VENDOR, 12)
#endif

static SceNetSockaddr convertP2PToPosix(const SceNetSockaddr *addr) {
    if (!addr) {
        return SceNetSockaddr{};
    }
    SceNetSockaddrIn result = *reinterpret_cast<const SceNetSockaddrIn *>(addr);
    // Convert ports from network to host order
    const uint16_t port = ntohs(result.sin_port);
    const uint16_t vport = ntohs(result.sin_vport);

    // Combine the two ports in host order, then convert back to network order
    result.sin_port = htons(port + vport);
    result.sin_vport = 0; // Clear virtual port since it's not used in Posix
    return std::bit_cast<SceNetSockaddr>(result);
}

static SceNetSockaddr convertPosixToP2P(const SceNetSockaddr *addr) {
    if (!addr) {
        return SceNetSockaddr{};
    }
    SceNetSockaddrIn result = *reinterpret_cast<const SceNetSockaddrIn *>(addr);
    // Attempt to recover original port and virtual port if port was previously combined
    const uint16_t port = ntohs(result.sin_port);
    if (port > SCE_NET_ADHOC_PORT) {
        const uint16_t vport = port - SCE_NET_ADHOC_PORT;
        result.sin_port = htons(SCE_NET_ADHOC_PORT);
        result.sin_vport = htons(vport);
    } else {
        result.sin_port = htons(port);
        result.sin_vport = 0;
    }
    return std::bit_cast<SceNetSockaddr>(result);
}

static int p2pSocketTypeToPosixSocketType(int type) {
    int hostSockType;
    switch (type) {
    case SCE_NET_SOCK_DGRAM_P2P:
        hostSockType = SOCK_DGRAM;
        break;
    case SCE_NET_SOCK_STREAM_P2P:
        hostSockType = SOCK_STREAM;
        break;
    default:
        hostSockType = -1;
    }
    return hostSockType;
}

P2PSocket::P2PSocket(int domain, int type, int protocol)
    : PosixSocket(domain, p2pSocketTypeToPosixSocketType(type), protocol) {
    sce_type = type;
#ifdef _WIN32
    // ss-delta: a datagram to a port nobody listens on (a peer that left, or a local copy that
    // is not running) makes Windows fail the socket's next receive with WSAECONNRESET; a Vita
    // ignores it
    if (type == SCE_NET_SOCK_DGRAM_P2P) {
        BOOL report = FALSE;
        DWORD returned = 0;
        WSAIoctl(sock, SIO_UDP_CONNRESET_, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
    }
#endif
}

SocketPtr P2PSocket::accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) {
    const auto res = std::dynamic_pointer_cast<PosixSocket>(PosixSocket::accept(addr, addrlen, err));
    if (!res)
        return nullptr;

    *addr = convertPosixToP2P(addr);
    return std::make_shared<P2PSocket>(res->sock, res->sce_type);
}

int P2PSocket::connect(const SceNetSockaddr *addr, unsigned int addrlen) {
    const auto p2p_addr = convertP2PToPosix(addr);
    return PosixSocket::connect(&p2p_addr, addrlen);
}

int P2PSocket::recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) {
    const auto res = PosixSocket::recv_packet(buf, len, flags, from, fromlen);
    if ((res > 0) && from)
        *from = convertPosixToP2P(from);

    return res;
}

int P2PSocket::send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) {
    auto p2p_to = convertP2PToPosix(to);
    auto &to_in = reinterpret_cast<SceNetSockaddrIn &>(p2p_to);
    if (to && net_utils::is_local_adhoc_broadcast(to_in.sin_addr.s_addr)) {
        // ss-delta, copies on one PC: a broadcast is a datagram to each other copy
        int res = static_cast<int>(len);
        for (int player = 1; player <= net_utils::LOCAL_ADHOC_MAX; player++) {
            if (player == net_utils::local_adhoc_player())
                continue;
            to_in.sin_addr.s_addr = net_utils::local_adhoc_addr(player);
            const int sent = PosixSocket::send_packet(msg, len, flags, &p2p_to, tolen);
            if (sent < 0)
                res = sent;
        }
        return res;
    }
    return PosixSocket::send_packet(msg, len, flags, &p2p_to, tolen);
}

int P2PSocket::bind(const SceNetSockaddr *addr, unsigned int addrlen) {
    auto p2p_addr = convertP2PToPosix(addr);
    // ss-delta, copies on one PC: each binds its own address, so their ports do not collide
    auto &addr_in = reinterpret_cast<SceNetSockaddrIn &>(p2p_addr);
    if (net_utils::local_adhoc_player() && addr_in.sin_addr.s_addr == htonl(INADDR_ANY))
        addr_in.sin_addr.s_addr = net_utils::local_adhoc_addr(net_utils::local_adhoc_player());
    return PosixSocket::bind(&p2p_addr, addrlen);
}

int P2PSocket::get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    const auto res = PosixSocket::get_peer_address(addr, addrlen);
    if (res == 0)
        *addr = convertPosixToP2P(addr);

    return res;
}

int P2PSocket::get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    const auto res = PosixSocket::get_socket_address(addr, addrlen);
    if (res == 0)
        *addr = convertPosixToP2P(addr);

    return res;
}
