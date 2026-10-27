/*
 * LANywhere: cliente del servidor de salas (docs/PROTOCOL.md).
 *
 * Mantiene la conexión de control (TCP) y la de datos (UDP) con el servidor mientras la consola está en una sala,
 * y guarda en una cola los paquetes que llegan para cada puerto virtual; bsd_mitm los entrega al juego en su
 * siguiente recvfrom.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms and conditions of
 * the GNU General Public License, version 2, as published by the Free Software Foundation.
 */
#pragma once
#include <stratosphere.hpp>

R_DEFINE_NAMESPACE_RESULT_MODULE(ams::mitm::lanywhere, 450);

namespace ams::mitm::lanywhere {

    R_DEFINE_ERROR_RESULT(RelayFailed, 1);  // el motivo legible queda en Status::error

    constexpr size_t CodeLength = 6;
    constexpr size_t NickLength = 32;
    constexpr size_t MaxMembers = 8;
    constexpr size_t MaxPayload = 1400;

    enum class RelayState : u8 { Idle = 0, Connecting = 1, InRoom = 2, Error = 3 };

    struct Member {
        bool present;
        u8 index;
        u32 ip;  // orden de host
        char nick[NickLength];
    };

    // Lo que ve la app "Jugar en línea" (y la prueba).
    struct Status {
        RelayState state;
        u8 my_index;
        u8 member_count;
        u8 padding;
        u32 my_ip;
        char code[8];
        u64 game_id;
        u32 packets_sent, packets_received;
        char error[64];
    };
    static_assert(sizeof(Status) == 0x60);

    // Un grupo abierto, resumido para la app.
    struct GroupSummary {
        char code[8];
        u64 game_id;      // localCommunicationId (en casi todos los juegos, su id de aplicación)
        u8 players, max_players;
        u8 padding[6];
        char host[40];    // nombre del anfitrión en el juego
    };
    static_assert(sizeof(GroupSummary) == 0x40);

    // 10.114.0.0/24: la red virtual de una sala.
    constexpr u32 VirtualNet = (10u << 24) | (114u << 16);
    constexpr bool IsVirtual(u32 ip) {
        return (ip & 0xFFFFFF00u) == VirtualNet || ip == 0xFFFFFFFFu;
    }

    // El nombre en la sala es siempre el apodo (config.ini "apodo="; uno al azar si no hay), nunca el nombre de
    // usuario de la consola.
    void Nickname(char *out, size_t size);
    Result CreateRoom(u64 game_id);
    Result JoinRoom(const char *code, u64 game_id);
    void LeaveRoom();
    void GetStatus(Status *out);
    bool InRoom();
    u32 MyIp();
    // Distinto en cada sala (del código): para que los grupos de salas distintas no compartan BSSID.
    u32 RoomTag();

    // Envía un paquete del juego a otro miembro (o a todos, si dst es broadcast).
    void SendData(u32 dst_ip, u16 src_port, u16 dst_port, const void *data, size_t size);

    // Saca de la cola el primer paquete para ese puerto: devuelve su tamaño (truncado a `size`) o -1 si no hay.
    ssize_t ReceiveData(u16 port, void *buffer, size_t size, u32 *src_ip, u16 *src_port);

    // Control de ldn (búsqueda, unión y sincronización de grupos) entre miembros de la sala, por la conexión TCP
    // con el servidor: llega entero y en orden. Cada mensaje es [LdnMsg][datos].
    enum class LdnMsg : u8 {
        Scan = 1,        // ¿quién tiene un grupo?                 (a todos)
        ScanResp = 2,    // NetworkInfo del grupo                  (al que buscó)
        Connect = 3,     // NodeInfo del que se une                (al anfitrión)
        Sync = 4,        // NetworkInfo actualizado                (del anfitrión a cada estación)
        Disconnect = 5,  // sale del grupo o lo cierra
        MemberLeft = 0xFF,  // generado aquí: el miembro salió de la sala
    };
    constexpr u8 AllMembers = 0xFF;
    constexpr size_t MaxControl = 0x500;

    // Grupos abiertos: el anfitrión publica su grupo de ldn (NetworkInfo; tamaño 0 lo retira) y quien busca recibe
    // los de su juego con sitio libre, con el código de su sala.
    bool PublicEnabled();  // config.ini "public=0" los desactiva
    void Publish(const void *network_info, size_t size);
    // Pregunta con una conexión aparte (no hace falta estar en una sala). Devuelve cuántos o -1.
    // from_app: la pide la lista de la app (no el juego al buscar grupo); el servidor lo cuenta aparte en el panel.
    int FindGroups(u64 local_communication_id, void (*found)(const char *code, const void *network_info, void *user), void *user,
                   bool from_app);

    // El grupo abierto elegido en la app: al buscar en el juego se pide también ese (aunque no esté entre los primeros)
    // y se muestra solo él. Caduca a los 10 minutos o al unirse. Código vacío: ninguno.
    void PreferGroup(const char *code);
    bool PreferredGroup(char *out, size_t size);

    // Registro de la sala en sdmc:/lanywhere/relay.log (hasta 256 KB).
    void Log(const char *fmt, ...);

    bool SendControl(u8 dest, LdnMsg msg, const void *data, size_t size);
    // Saca el siguiente mensaje de control: tamaño de los datos (sin el LdnMsg) o -1 si no hay.
    ssize_t TakeControl(u8 *from, LdnMsg *msg, void *buffer, size_t size);

    // Índice del miembro con esa IP virtual, o -1.
    constexpr int MemberOfIp(u32 ip) {
        return (ip & 0xFFFFFF00u) == VirtualNet && (ip & 0xFF) >= 1 && (ip & 0xFF) <= MaxMembers ? static_cast<int>(ip & 0xFF) - 1 : -1;
    }

}
