/*
 * LANywhere: servicio "lanywhr" para que una app (la de "Jugar en línea", o la de pruebas) cree salas, se una
 * a ellas por código, salga y consulte el estado.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms and conditions of
 * the GNU General Public License, version 2, as published by the Free Software Foundation.
 */
#pragma once
#include <stratosphere.hpp>
#include "lanywhere_relay.hpp"

#define AMS_ILANYWHERE_SERVICE(C, H)                                                                                                     \
    AMS_SF_METHOD_INFO(C, H, 0, Result, CreateRoom, (u64 game_id, const ams::sf::InBuffer &nick), (game_id, nick))                 \
    AMS_SF_METHOD_INFO(C, H, 1, Result, JoinRoom,   (u64 game_id, const ams::sf::InBuffer &code, const ams::sf::InBuffer &nick),    \
                                                    (game_id, code, nick))                                                         \
    AMS_SF_METHOD_INFO(C, H, 2, Result, LeaveRoom,  (), ())                                                                         \
    AMS_SF_METHOD_INFO(C, H, 3, Result, GetStatus,  (ams::sf::Out<ams::mitm::lanywhere::Status> out), (out))               AMS_SF_METHOD_INFO(C, H, 4, Result, ListGroups, (ams::sf::Out<s32> count, const ams::sf::OutBuffer &out), (count, out))     AMS_SF_METHOD_INFO(C, H, 5, Result, GetNickname, (const ams::sf::OutBuffer &out), (out))

AMS_SF_DEFINE_INTERFACE(ams::mitm::lanywhere, ILanywhereService, AMS_ILANYWHERE_SERVICE, 0x594E414C)

namespace ams::mitm::lanywhere {

    class LanywhereService {
        public:
            Result CreateRoom(u64 game_id, const sf::InBuffer &nick);
            Result JoinRoom(u64 game_id, const sf::InBuffer &code, const sf::InBuffer &nick);
            Result LeaveRoom();
            Result GetStatus(sf::Out<Status> out);
            Result ListGroups(sf::Out<s32> count, const sf::OutBuffer &out);  // count -1: sin servidor
            Result GetNickname(const sf::OutBuffer &out);
    };
    static_assert(IsILanywhereService<LanywhereService>);

}
