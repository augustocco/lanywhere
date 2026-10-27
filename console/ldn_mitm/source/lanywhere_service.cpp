/*
 * LANywhere: servicio "lanywhr" (ver lanywhere_service.hpp).
 *
 * This program is free software; you can redistribute it and/or modify it under the terms and conditions of
 * the GNU General Public License, version 2, as published by the Free Software Foundation.
 */
#include "lanywhere_service.hpp"
#include <cstring>

namespace ams::mitm::lanywhere {

    namespace {

        // Copia un búfer de texto del cliente a una cadena terminada en cero.
        void CopyText(char *dst, size_t dst_size, const sf::InBuffer &src) {
            const size_t n = std::min(src.GetSize(), dst_size - 1);
            std::memcpy(dst, src.GetPointer(), n);
            dst[n] = '\0';
        }

    }

    Result LanywhereService::CreateRoom(u64 game_id, const sf::InBuffer &nick) {
        AMS_UNUSED(nick);  // el nombre es el apodo de config.ini
        R_RETURN(lanywhere::CreateRoom(game_id));
    }

    Result LanywhereService::JoinRoom(u64 game_id, const sf::InBuffer &code, const sf::InBuffer &nick) {
        AMS_UNUSED(nick);
        char room[CodeLength + 1];
        CopyText(room, sizeof(room), code);
        R_RETURN(lanywhere::JoinRoom(room, game_id));
    }

    Result LanywhereService::LeaveRoom() {
        lanywhere::LeaveRoom();
        R_SUCCEED();
    }

    namespace {

        struct GroupList {
            GroupSummary *out;
            size_t capacity, count;
        };

        void AddGroup(const char *code, const void *data, void *user) {
            GroupList *list = static_cast<GroupList *>(user);
            if (list->count >= list->capacity) {
                return;
            }
            // NetworkInfo de ldn: id del juego en 0, máximo y jugadores en 0x66 y 0x67, nombre del nodo 0 en 0x74.
            const u8 *info = static_cast<const u8 *>(data);
            GroupSummary &g = list->out[list->count++];
            std::memset(std::addressof(g), 0, sizeof(g));
            util::Strlcpy(g.code, code, sizeof(g.code));
            std::memcpy(std::addressof(g.game_id), info, sizeof(g.game_id));
            g.max_players = info[0x66];
            g.players = info[0x67];
            std::memcpy(g.host, info + 0x68 + 12, 32);
            g.host[32] = '\0';
        }

    }

    Result LanywhereService::ListGroups(sf::Out<s32> count, const sf::OutBuffer &out) {
        GroupList list = { reinterpret_cast<GroupSummary *>(out.GetPointer()), out.GetSize() / sizeof(GroupSummary), 0 };
        const int found = lanywhere::FindGroups(0, AddGroup, std::addressof(list), true);
        count.SetValue(found < 0 ? -1 : static_cast<s32>(list.count));
        R_SUCCEED();
    }

    Result LanywhereService::PreferGroup(const sf::InBuffer &code) {
        char room[CodeLength + 1];
        CopyText(room, sizeof(room), code);
        lanywhere::PreferGroup(room);
        R_SUCCEED();
    }

    Result LanywhereService::GetNickname(const sf::OutBuffer &out) {
        if (out.GetSize() > 0) {
            lanywhere::Nickname(reinterpret_cast<char *>(out.GetPointer()), out.GetSize());
        }
        R_SUCCEED();
    }

    Result LanywhereService::GetStatus(sf::Out<Status> out) {
        Status status;
        lanywhere::GetStatus(std::addressof(status));
        out.SetValue(status);
        R_SUCCEED();
    }

}
