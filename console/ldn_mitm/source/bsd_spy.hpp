/*
 * LANywhere: observación (sin cambios) de los sockets de los juegos, servicio bsd:u.
 *
 * Primer paso del plan: saber qué hace un juego en modo inalámbrico local (UDP/TCP, direcciones, puertos,
 * volumen) antes de redirigir su tráfico por el servidor. Cada comando se reenvía tal cual al servicio real;
 * los demás comandos de bsd:u los reenvía el propio servidor mitm.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms and conditions of
 * the GNU General Public License, version 2, as published by the Free Software Foundation.
 */
#pragma once
#include <stratosphere.hpp>

#define AMS_IBSD_SPY(C, H)                                                                                                                                  \
    AMS_SF_METHOD_INFO(C, H,  2, Result, Socket,  (ams::sf::Out<s32> out_ret, ams::sf::Out<s32> out_errno, s32 domain, s32 type, s32 protocol),           \
                                                  (out_ret, out_errno, domain, type, protocol))                                                         \
    AMS_SF_METHOD_INFO(C, H,  6, Result, Poll,    (ams::sf::Out<s32> out_ret, ams::sf::Out<s32> out_errno, u32 nfds, s32 timeout,                        \
                                                   const ams::sf::InAutoSelectBuffer &fds_in, const ams::sf::OutAutoSelectBuffer &fds_out),             \
                                                  (out_ret, out_errno, nfds, timeout, fds_in, fds_out))                                                 \
    AMS_SF_METHOD_INFO(C, H,  8, Result, Recv,    (ams::sf::Out<s32> out_ret, ams::sf::Out<s32> out_errno, s32 fd, s32 flags,                            \
                                                   const ams::sf::OutAutoSelectBuffer &data),                                                           \
                                                  (out_ret, out_errno, fd, flags, data))                                                                \
    AMS_SF_METHOD_INFO(C, H,  9, Result, RecvFrom,(ams::sf::Out<s32> out_ret, ams::sf::Out<s32> out_errno, ams::sf::Out<u32> out_addrlen, s32 fd,       \
                                                   s32 flags, const ams::sf::OutAutoSelectBuffer &data, const ams::sf::OutAutoSelectBuffer &addr),      \
                                                  (out_ret, out_errno, out_addrlen, fd, flags, data, addr))                                             \
    AMS_SF_METHOD_INFO(C, H, 20, Result, Fcntl,   (ams::sf::Out<s32> out_ret, ams::sf::Out<s32> out_errno, s32 fd, s32 cmd, s32 arg),                    \
                                                  (out_ret, out_errno, fd, cmd, arg))                                                                   \
    AMS_SF_METHOD_INFO(C, H, 21, Result, SetSockOpt, (ams::sf::Out<s32> out_ret, ams::sf::Out<s32> out_errno, s32 fd, s32 level, s32 optname,           \
                                                   const ams::sf::InAutoSelectBuffer &optval),                                                          \
                                                  (out_ret, out_errno, fd, level, optname, optval))                                                     \
    AMS_SF_METHOD_INFO(C, H, 11, Result, SendTo,  (ams::sf::Out<s32> out_ret, ams::sf::Out<s32> out_errno, s32 fd, s32 flags,                            \
                                                   const ams::sf::InAutoSelectBuffer &data, const ams::sf::InAutoSelectBuffer &addr),                   \
                                                  (out_ret, out_errno, fd, flags, data, addr))                                                          \
    AMS_SF_METHOD_INFO(C, H, 13, Result, Bind,    (ams::sf::Out<s32> out_ret, ams::sf::Out<s32> out_errno, s32 fd, const ams::sf::InAutoSelectBuffer &addr), \
                                                  (out_ret, out_errno, fd, addr))                                                                       \
    AMS_SF_METHOD_INFO(C, H, 14, Result, Connect, (ams::sf::Out<s32> out_ret, ams::sf::Out<s32> out_errno, s32 fd, const ams::sf::InAutoSelectBuffer &addr), \
                                                  (out_ret, out_errno, fd, addr))                                                                       \
    AMS_SF_METHOD_INFO(C, H, 18, Result, Listen,  (ams::sf::Out<s32> out_ret, ams::sf::Out<s32> out_errno, s32 fd, s32 backlog),                         \
                                                  (out_ret, out_errno, fd, backlog))                                                                    \
    AMS_SF_METHOD_INFO(C, H, 26, Result, Close,   (ams::sf::Out<s32> out_ret, ams::sf::Out<s32> out_errno, s32 fd), (out_ret, out_errno, fd))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::bsd, IBsdSpy, AMS_IBSD_SPY, 0x5357424F)

namespace ams::mitm::bsd {

    class BsdSpyService : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;

            // Solo los juegos (aplicaciones): ni el sistema ni este mismo sysmodule.
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                return client_info.program_id.value >= 0x0100000000010000ul;
            }

            Result Socket(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 domain, s32 type, s32 protocol);
            Result Poll(sf::Out<s32> out_ret, sf::Out<s32> out_errno, u32 nfds, s32 timeout, const sf::InAutoSelectBuffer &fds_in,
                        const sf::OutAutoSelectBuffer &fds_out);
            Result Recv(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, s32 flags, const sf::OutAutoSelectBuffer &data);
            Result RecvFrom(sf::Out<s32> out_ret, sf::Out<s32> out_errno, sf::Out<u32> out_addrlen, s32 fd, s32 flags,
                            const sf::OutAutoSelectBuffer &data, const sf::OutAutoSelectBuffer &addr);
            Result Fcntl(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, s32 cmd, s32 arg);
            Result SetSockOpt(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, s32 level, s32 optname, const sf::InAutoSelectBuffer &optval);
            Result SendTo(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, s32 flags, const sf::InAutoSelectBuffer &data,
                          const sf::InAutoSelectBuffer &addr);
            Result Bind(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, const sf::InAutoSelectBuffer &addr);
            Result Connect(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, const sf::InAutoSelectBuffer &addr);
            Result Listen(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, s32 backlog);
            Result Close(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd);
    };
    static_assert(IsIBsdSpy<BsdSpyService>);

}
