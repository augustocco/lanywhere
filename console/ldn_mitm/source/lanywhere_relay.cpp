/*
 * LANywhere: cliente del servidor de salas (ver lanywhere_relay.hpp y docs/PROTOCOL.md).
 *
 * This program is free software; you can redistribute it and/or modify it under the terms and conditions of
 * the GNU General Public License, version 2, as published by the Free Software Foundation.
 */
#include "lanywhere_relay.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace ams::mitm::lanywhere {

    namespace {

        constexpr const char DefaultServer[] = "129.38.114.182";
        constexpr u16 DefaultPort = 11460;
        constexpr const char ConfigPath[] = "sdmc:/lanywhere/config.ini";
        constexpr const char LogPath[] = "sdmc:/lanywhere/relay.log";
        constexpr u32 UdpMagic = 0x594E414C;
        constexpr int ConnectTimeoutMs = 5000;
        constexpr TimeSpan KeepAliveInterval = TimeSpan::FromSeconds(5);
        constexpr TimeSpan ReconnectWindow = TimeSpan::FromSeconds(55);  // el servidor guarda el puesto 60 s
        constexpr size_t QueueSize = 128;

        enum : u8 { T_Create = 0x01, T_Join = 0x02, T_Leave = 0x03, T_Relay = 0x04, T_Ping = 0x05, T_Resume = 0x06, T_Publish = 0x07, T_Find = 0x08,
                    T_Room = 0x81, T_MemberJoined = 0x82, T_MemberLeft = 0x83, T_RelayIn = 0x84, T_Error = 0x85, T_Pong = 0x86, T_Groups = 0x87 };
        enum : u8 { U_Register = 0x01, U_Data = 0x02, U_KeepAlive = 0x03, U_Registered = 0x81, U_DataIn = 0x82 };

        struct Packet { u16 dst_port; u16 src_port; u32 src_ip; u16 size; u8 data[MaxPayload]; };
        struct Control { u8 from; LdnMsg msg; u16 size; u8 data[MaxControl]; };
        constexpr size_t ControlQueueSize = 16;

        constinit os::SdkMutex g_lock;        // estado de la sala
        constinit os::SdkMutex g_queue_lock;  // cola de paquetes
        constinit os::SdkMutex g_send_lock;   // escrituras en el TCP (hilo de la sala e hilos de ldn)
        RelayState g_state = RelayState::Idle;
        char g_code[CodeLength + 1];
        char g_error[64];
        u64 g_game_id;
        u8 g_my_index;
        u32 g_my_ip, g_token;
        Member g_members[MaxMembers];
        u32 g_sent, g_received;
        int g_tcp = -1, g_udp = -1;
        sockaddr_in g_server;
        os::Tick g_last_keepalive;
        bool g_public = true;  // grupos abiertos (config.ini "public=0" los desactiva)
        char g_nick[NickLength];  // apodo (config.ini "apodo="); nunca el nombre de usuario de la consola

        Packet g_queue[QueueSize];
        size_t g_queue_head, g_queue_count;
        Control g_controls[ControlQueueSize];
        size_t g_control_head, g_control_count;

        alignas(os::ThreadStackAlignment) u8 g_thread_stack[0x4000];
        os::ThreadType g_thread;
        bool g_thread_started;

        constinit os::SdkMutex g_log_lock;

        void SetError(const char *msg) {
            util::Strlcpy(g_error, msg, sizeof(g_error));
            g_state = RelayState::Error;
        }

        u16 le16(const u8 *p) { return p[0] | p[1] << 8; }
        u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<u32>(p[3]) << 24; }
        u64 le64(const u8 *p) { return le32(p) | static_cast<u64>(le32(p + 4)) << 32; }
        void put16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
        void put32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
        void put64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = v >> (8 * i); }

        // Servidor: sdmc:/lanywhere/config.ini con "server=IP" y "port=N"; si no, el de LANywhere.
        void LoadServer() {
            char host[64];
            util::Strlcpy(host, DefaultServer, sizeof(host));
            u16 port = DefaultPort;
            g_nick[0] = '\0';
            fs::FileHandle file;
            if (R_SUCCEEDED(fs::OpenFile(std::addressof(file), ConfigPath, fs::OpenMode_Read))) {
                char text[512] = {};
                size_t read = 0;
                if (R_SUCCEEDED(fs::ReadFile(std::addressof(read), file, 0, text, sizeof(text) - 1))) {
                    text[read] = '\0';
                    for (char *line = std::strtok(text, "\r\n"); line; line = std::strtok(nullptr, "\r\n")) {
                        if (std::strncmp(line, "server=", 7) == 0) util::Strlcpy(host, line + 7, sizeof(host));
                        if (std::strncmp(line, "port=", 5) == 0) port = static_cast<u16>(std::atoi(line + 5));
                        if (std::strncmp(line, "public=", 7) == 0) g_public = line[7] != '0';
                        if (std::strncmp(line, "apodo=", 6) == 0) util::Strlcpy(g_nick, line + 6, sizeof(g_nick));
                    }
                }
                fs::CloseFile(file);
            }
            std::memset(std::addressof(g_server), 0, sizeof(g_server));
            g_server.sin_family = AF_INET;
            g_server.sin_port = htons(port);
            inet_pton(AF_INET, host, std::addressof(g_server.sin_addr));
        }

        void CloseSockets() {
            std::scoped_lock lk(g_send_lock);
            if (g_tcp >= 0) { ::close(g_tcp); g_tcp = -1; }
            if (g_udp >= 0) { ::close(g_udp); g_udp = -1; }
        }

        bool SendFrameTo(int fd, u8 type, const void *body, size_t size, const void *extra, size_t extra_size) {
            u8 frame[3 + 2 + MaxControl];
            if (size + extra_size > sizeof(frame) - 3 || fd < 0) {
                return false;
            }
            put16(frame, static_cast<u16>(size + extra_size + 1));
            frame[2] = type;
            if (size) std::memcpy(frame + 3, body, size);
            if (extra_size) std::memcpy(frame + 3 + size, extra, extra_size);
            const size_t total = 3 + size + extra_size;
            for (size_t sent = 0; sent < total; ) {
                const ssize_t n = ::send(fd, frame + sent, total - sent, 0);
                if (n <= 0) return false;
                sent += n;
            }
            return true;
        }

        bool SendFrame(u8 type, const void *body, size_t size, const void *extra = nullptr, size_t extra_size = 0) {
            std::scoped_lock lk(g_send_lock);
            return SendFrameTo(g_tcp, type, body, size, extra, extra_size);
        }

        void EnqueueControl(u8 from, LdnMsg msg, const u8 *data, size_t size) {
            if (size > MaxControl) return;
            std::scoped_lock lk(g_queue_lock);
            if (g_control_count == ControlQueueSize) {
                g_control_head = (g_control_head + 1) % ControlQueueSize;
                g_control_count--;
            }
            Control &c = g_controls[(g_control_head + g_control_count) % ControlQueueSize];
            c.from = from;
            c.msg = msg;
            c.size = static_cast<u16>(size);
            if (size) std::memcpy(c.data, data, size);
            g_control_count++;
        }

        // Lee una trama completa (bloqueando como mucho timeout_ms). Devuelve su tamaño de cuerpo o -1.
        ssize_t ReadFrame(int fd, u8 *type, u8 *body, size_t body_size, int timeout_ms) {
            u8 head[3];
            size_t got = 0;
            while (got < sizeof(head)) {
                pollfd p = { fd, POLLIN, 0 };
                if (::poll(std::addressof(p), 1, timeout_ms) <= 0) return -1;
                const ssize_t n = ::recv(fd, head + got, sizeof(head) - got, 0);
                if (n <= 0) return -1;
                got += n;
            }
            const size_t len = le16(head) - 1;
            *type = head[2];
            if (len > body_size) return -1;
            got = 0;
            while (got < len) {
                pollfd p = { fd, POLLIN, 0 };
                if (::poll(std::addressof(p), 1, timeout_ms) <= 0) return -1;
                const ssize_t n = ::recv(fd, body + got, len - got, 0);
                if (n <= 0) return -1;
                got += n;
            }
            return static_cast<ssize_t>(len);
        }

        void UdpSendTo(int fd, u32 token, u8 type, const void *body, size_t size) {
            u8 buf[9 + 8 + MaxPayload];
            put32(buf, UdpMagic);
            buf[4] = type;
            put32(buf + 5, token);
            if (size) std::memcpy(buf + 9, body, size);
            ::sendto(fd, buf, 9 + size, 0, reinterpret_cast<const sockaddr *>(std::addressof(g_server)), sizeof(g_server));
        }

        void UdpSend(u8 type, const void *body, size_t size) {
            UdpSendTo(g_udp, g_token, type, body, size);
        }

        void AddMember(const u8 *m) {
            const u8 index = m[0];
            if (index >= MaxMembers) return;
            Member &dst = g_members[index];
            dst.present = true;
            dst.index = index;
            dst.ip = le32(m + 1);
            std::memcpy(dst.nick, m + 5, NickLength);
            dst.nick[NickLength - 1] = '\0';
        }

        void Enqueue(const u8 *msg, size_t size) {
            // DATA del servidor: magic, tipo, ip origen, puerto origen, puerto destino, carga.
            if (size < 13) return;
            std::scoped_lock lk(g_queue_lock);
            if (g_queue_count == QueueSize) {  // cola llena: se descarta el más antiguo (como una red saturada)
                g_queue_head = (g_queue_head + 1) % QueueSize;
                g_queue_count--;
            }
            Packet &p = g_queue[(g_queue_head + g_queue_count) % QueueSize];
            p.src_ip = le32(msg + 5);
            p.src_port = le16(msg + 9);
            p.dst_port = le16(msg + 11);
            p.size = static_cast<u16>(std::min(size - 13, MaxPayload));
            std::memcpy(p.data, msg + 13, p.size);
            g_queue_count++;
        }

        void HandleFrame(u8 type, const u8 *body, size_t size) {
            std::scoped_lock lk(g_lock);
            switch (type) {
                case T_MemberJoined: if (size >= 37) AddMember(body); break;
                case T_MemberLeft:
                    if (size >= 1 && body[0] < MaxMembers) {
                        g_members[body[0]].present = false;
                        EnqueueControl(body[0], LdnMsg::MemberLeft, nullptr, 0);
                    }
                    break;
                case T_RelayIn:
                    if (size >= 2) {
                        Log("ldn <- miembro %u: mensaje %u (%zu bytes)", body[0], body[1], size - 2);
                        EnqueueControl(body[0], static_cast<LdnMsg>(body[1]), body + 2, size - 2);
                    }
                    break;
                default:             break;  // ERROR, PONG
            }
        }

        // Una conexión ya establecida con el servidor (CREATE, JOIN o RESUME contestado con ROOM y UDP registrado).
        struct Session {
            int tcp, udp;
            bool no_seat;  // RESUME rechazado: el servidor ya no guarda el puesto
            size_t reply_size;
            u8 reply[24 + MaxMembers * 37 + 64];
            char error[64];
        };

        // Conexión TCP con el servidor (con tiempo límite): fd o -1 y el motivo.
        int ConnectServer(const char **err) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) { *err = "no se pudo crear el socket"; return -1; }
            const int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, std::addressof(one), sizeof(one));
            // connect no bloqueante con tiempo límite
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            if (::connect(fd, reinterpret_cast<const sockaddr *>(std::addressof(g_server)), sizeof(g_server)) != 0 && errno != EINPROGRESS) {
                ::close(fd);
                *err = "no se pudo conectar con el servidor";
                return -1;
            }
            pollfd p = { fd, POLLOUT, 0 };
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (::poll(std::addressof(p), 1, ConnectTimeoutMs) <= 0 ||
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, std::addressof(so_error), std::addressof(len)) != 0 || so_error != 0) {
                ::close(fd);
                *err = "el servidor no responde";
                return -1;
            }
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
            return fd;
        }

        // Conecta y negocia sin tocar el estado global (puede tardar segundos y el juego consulta InRoom() sin parar).
        bool Handshake(u8 type, const u8 *body, size_t size, Session *s) {
            s->tcp = s->udp = -1;
            s->no_seat = false;
            s->error[0] = '\0';
            auto fail = [s](const char *msg) {
                if (s->tcp >= 0) ::close(s->tcp);
                if (s->udp >= 0) ::close(s->udp);
                s->tcp = s->udp = -1;
                if (!s->error[0]) util::Strlcpy(s->error, msg, sizeof(s->error));
                return false;
            };

            const char *err = nullptr;
            s->tcp = ConnectServer(std::addressof(err));
            if (s->tcp < 0) return fail(err);

            u8 rtype = 0;
            ssize_t n = -1;
            if (SendFrameTo(s->tcp, type, body, size, nullptr, 0)) {
                n = ReadFrame(s->tcp, std::addressof(rtype), s->reply, sizeof(s->reply), ConnectTimeoutMs);
            }
            if (n < 0) return fail("el servidor no contestó");
            if (rtype == T_Error && n >= 1) {
                const u8 code = s->reply[0];
                s->no_seat = code == 5;
                char msg[64] = {};
                std::memcpy(msg, s->reply + 1, std::min<size_t>(n - 1, sizeof(msg) - 1));
                return fail(code == 1 ? "la sala no existe" : code == 2 ? "la sala está llena" :
                            code == 3 ? "la sala es de otro juego" : msg);
            }
            if (rtype != T_Room || n < 24) return fail("respuesta inesperada");
            s->reply_size = n;

            // Datos: registrar el UDP (el servidor contesta REGISTERED).
            const u32 token = le32(s->reply + 11);
            s->udp = ::socket(AF_INET, SOCK_DGRAM, 0);
            bool registered = false;
            for (int i = 0; i < 20 && s->udp >= 0 && !registered; i++) {
                UdpSendTo(s->udp, token, U_Register, nullptr, 0);
                pollfd up = { s->udp, POLLIN, 0 };
                if (::poll(std::addressof(up), 1, 150) > 0) {
                    u8 r[16];
                    const ssize_t rn = ::recv(s->udp, r, sizeof(r), 0);
                    registered = rn >= 5 && le32(r) == UdpMagic && r[4] == U_Registered;
                }
            }
            if (!registered) return fail("el servidor no recibe datos (UDP)");
            return true;
        }

        // Adopta la sesión (con g_lock tomado). Los miembros que se fueron mientras no había conexión se avisan a ldn.
        void Install(const Session &s) {
            {
                std::scoped_lock lk(g_send_lock);
                g_tcp = s.tcp;
                g_udp = s.udp;
            }
            const u8 *reply = s.reply;
            std::memcpy(g_code, reply, CodeLength);
            g_code[CodeLength] = '\0';
            g_my_index = reply[6];
            g_my_ip = le32(reply + 7);
            g_token = le32(reply + 11);
            g_game_id = le64(reply + 15);
            bool before[MaxMembers];
            for (size_t i = 0; i < MaxMembers; i++) {
                before[i] = g_members[i].present;
            }
            std::memset(g_members, 0, sizeof(g_members));
            for (size_t i = 0, count = reply[23]; i < count && 24 + (i + 1) * 37 <= s.reply_size; i++) {
                AddMember(reply + 24 + i * 37);
            }
            for (size_t i = 0; i < MaxMembers; i++) {
                if (before[i] && !g_members[i].present) {
                    EnqueueControl(static_cast<u8>(i), LdnMsg::MemberLeft, nullptr, 0);
                }
            }
        }

        // Se cortó la conexión (por ejemplo, el juego activó el modo local y la consola reconectó el Wi-Fi): volver
        // al mismo puesto con RESUME mientras el servidor lo guarda. Falso si no se pudo o se salió de la sala.
        bool Reconnect() {
            CloseSockets();
            u8 body[4 + CodeLength];
            {
                std::scoped_lock lk(g_lock);
                put32(body, g_token);
                std::memcpy(body + 4, g_code, CodeLength);
            }
            const os::Tick start = os::GetSystemTick();
            while ((os::GetSystemTick() - start).ToTimeSpan() < ReconnectWindow) {
                {
                    std::scoped_lock lk(g_lock);
                    if (g_state != RelayState::InRoom) return false;
                    util::Strlcpy(g_error, "reconectando con el servidor", sizeof(g_error));
                }
                Session s;
                if (Handshake(T_Resume, body, sizeof(body), std::addressof(s))) {
                    std::scoped_lock lk(g_lock);
                    if (g_state != RelayState::InRoom) {
                        ::close(s.tcp);
                        ::close(s.udp);
                        return false;
                    }
                    Install(s);
                    g_error[0] = '\0';
                    g_last_keepalive = os::GetSystemTick();
                    return true;
                }
                Log("RESUME falló: %s", s.error);
                if (s.no_seat) return false;
                os::SleepThread(TimeSpan::FromSeconds(2));
            }
            return false;
        }

        void Worker(void *) {
            u8 buf[9 + 8 + MaxPayload + 64];
            while (true) {
                {
                    std::scoped_lock lk(g_lock);
                    if (g_state != RelayState::InRoom) break;
                }
                bool lost = false;
                pollfd p[2] = { { g_tcp, POLLIN, 0 }, { g_udp, POLLIN, 0 } };
                const int rc = ::poll(p, 2, 1000);
                if (rc < 0) {
                    lost = true;
                } else {
                    if (p[1].revents & POLLIN) {
                        const ssize_t n = ::recv(g_udp, buf, sizeof(buf), 0);
                        if (n >= 13 && le32(buf) == UdpMagic && buf[4] == U_DataIn) {
                            Enqueue(buf, n);
                            std::scoped_lock lk(g_lock);
                            g_received++;
                        }
                    }
                    if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                        u8 type;
                        const ssize_t n = ReadFrame(g_tcp, std::addressof(type), buf, sizeof(buf), 2000);
                        if (n < 0) {
                            lost = true;
                        } else {
                            HandleFrame(type, buf, n);
                        }
                    }
                }
                if (lost) {
                    Log("conexión perdida (poll %d); reconectando", rc);
                    if (Reconnect()) { Log("reconectado a la sala %s", g_code); continue; }
                    Log("no se pudo reconectar");
                    std::scoped_lock lk(g_lock);
                    if (g_state == RelayState::InRoom) SetError("se perdió la conexión con el servidor");
                    break;
                }
                if ((os::GetSystemTick() - g_last_keepalive).ToTimeSpan() >= KeepAliveInterval) {
                    g_last_keepalive = os::GetSystemTick();
                    if (!SendFrame(T_Ping, nullptr, 0)) {
                        Log("no se pudo enviar PING; reconectando");
                        if (Reconnect()) { Log("reconectado a la sala %s", g_code); continue; }
                        Log("no se pudo reconectar");
                        std::scoped_lock lk(g_lock);
                        if (g_state == RelayState::InRoom) SetError("se perdió la conexión con el servidor");
                        break;
                    }
                    UdpSend(U_KeepAlive, nullptr, 0);
                }
            }
            CloseSockets();
        }

        // Conecta, envía CREATE o JOIN y espera ROOM; luego registra el UDP y arranca el hilo.
        Result Enter(u8 type, const u8 *body, size_t size) {
            LeaveRoom();
            {
                std::scoped_lock lk(g_lock);
                g_state = RelayState::Connecting;
                g_error[0] = '\0';
                LoadServer();
            }
            Session s;
            const bool ok = Handshake(type, body, size, std::addressof(s));
            std::scoped_lock lk(g_lock);
            if (!ok) {
                Log("entrar en la sala falló: %s", s.error);
                SetError(s.error);
                R_THROW(ResultRelayFailed());
            }
            std::memset(g_members, 0, sizeof(g_members));
            g_sent = g_received = 0;
            {
                std::scoped_lock qlk(g_queue_lock);
                g_queue_head = g_queue_count = 0;
                g_control_head = g_control_count = 0;
            }
            Install(s);
            Log("en la sala %s como miembro %u", g_code, g_my_index);
            g_state = RelayState::InRoom;
            g_last_keepalive = os::GetSystemTick();
            R_ABORT_UNLESS(os::CreateThread(std::addressof(g_thread), Worker, nullptr, g_thread_stack, sizeof(g_thread_stack), 10));
            os::SetThreadNamePointer(std::addressof(g_thread), "lanywhere::Relay");
            os::StartThread(std::addressof(g_thread));
            g_thread_started = true;
            R_SUCCEED();
        }

    }

    void Nickname(char *out, size_t size) {
        std::scoped_lock lk(g_lock);
        LoadServer();
        if (!g_nick[0]) {
            // Primera vez: uno al azar, amable y sin datos de nadie; la app permite cambiarlo.
            static const char *const animals[] = { "Tigre", "Zorro", "Panda", "Lobo", "Koala", "Gato", "Conejo", "Tortuga",
                                                   "Pulpo", "Mapache", "Puma", "Jaguar", "Nutria", "Oso", "Pato", "Gallo" };
            static const char *const traits[] = { "Veloz", "Valiente", "Feliz", "Turbo", "Estelar", "Audaz", "Genial",
                                                  "Ninja", "Fugaz", "Rebelde", "Gigante", "Brillante" };
            u8 r[3];
            os::GenerateRandomBytes(r, sizeof(r));
            util::SNPrintf(g_nick, sizeof(g_nick), "%s %s %u", animals[r[0] % 16], traits[r[1] % 12], 10 + r[2] % 90);
            char line[NickLength + 16];
            const int len = util::SNPrintf(line, sizeof(line), "\napodo=%s\n", g_nick);
            static_cast<void>(fs::CreateDirectory("sdmc:/lanywhere"));
            fs::FileHandle file;
            if (R_FAILED(fs::OpenFile(std::addressof(file), ConfigPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) {
                static_cast<void>(fs::CreateFile(ConfigPath, 0));
            }
            if (R_SUCCEEDED(fs::OpenFile(std::addressof(file), ConfigPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) {
                s64 at = 0;
                if (R_SUCCEEDED(fs::GetFileSize(std::addressof(at), file))) {
                    static_cast<void>(fs::WriteFile(file, at, line + (at == 0), len - (at == 0), fs::WriteOption::Flush));
                }
                fs::CloseFile(file);
            }
        }
        util::Strlcpy(out, g_nick, size);
    }

    Result CreateRoom(u64 game_id) {
        u8 body[8 + NickLength] = {};
        put64(body, game_id);
        Nickname(reinterpret_cast<char *>(body + 8), NickLength);
        R_RETURN(Enter(T_Create, body, sizeof(body)));
    }

    Result JoinRoom(const char *code, u64 game_id) {
        u8 body[CodeLength + NickLength + 8] = {};
        std::memcpy(body, code, std::min(std::strlen(code), CodeLength));
        Nickname(reinterpret_cast<char *>(body + CodeLength), NickLength);
        put64(body + CodeLength + NickLength, game_id);
        R_RETURN(Enter(T_Join, body, sizeof(body)));
    }

    void LeaveRoom() {
        {
            std::scoped_lock lk(g_lock);
            if (g_state == RelayState::InRoom) {
                SendFrame(T_Leave, nullptr, 0);
            }
            g_state = RelayState::Idle;
        }
        // El hilo sale en menos de 1 s (deja de estar en la sala) y cierra los sockets. Se espera siempre, también
        // si terminó por un error: si no, podría cerrar los sockets de la sala siguiente.
        if (g_thread_started) {
            os::WaitThread(std::addressof(g_thread));
            os::DestroyThread(std::addressof(g_thread));
            g_thread_started = false;
        }
        std::scoped_lock lk(g_lock);
        CloseSockets();
    }

    void GetStatus(Status *out) {
        std::scoped_lock lk(g_lock);
        std::memset(out, 0, sizeof(*out));
        out->state = g_state;
        out->my_index = g_my_index;
        for (const auto &m : g_members) {
            out->member_count += m.present;
        }
        out->my_ip = g_my_ip;
        std::memcpy(out->code, g_code, CodeLength + 1);
        out->game_id = g_game_id;
        out->packets_sent = g_sent;
        out->packets_received = g_received;
        util::Strlcpy(out->error, g_error, sizeof(out->error));
    }

    bool InRoom() {
        std::scoped_lock lk(g_lock);
        return g_state == RelayState::InRoom;
    }

    u32 RoomTag() {
        std::scoped_lock lk(g_lock);
        u32 h = 2166136261u;  // FNV-1a
        for (size_t i = 0; i < CodeLength; i++) {
            h = (h ^ static_cast<u8>(g_code[i])) * 16777619u;
        }
        return h;
    }

    u32 MyIp() {
        std::scoped_lock lk(g_lock);
        return g_my_ip;
    }

    void SendData(u32 dst_ip, u16 src_port, u16 dst_port, const void *data, size_t size) {
        if (size > MaxPayload) {
            return;  // el juego no envía más que el MTU; por si acaso no se fragmenta
        }
        u8 body[8 + MaxPayload];
        put32(body, dst_ip == 0xFFFFFFFFu ? (VirtualNet | 0xFF) : dst_ip);
        put16(body + 4, src_port);
        put16(body + 6, dst_port);
        std::memcpy(body + 8, data, size);
        std::scoped_lock lk(g_lock);
        if (g_state == RelayState::InRoom) {
            UdpSend(U_Data, body, 8 + size);
            g_sent++;
        }
    }

    void Log(const char *fmt, ...) {
        char line[200];
        std::va_list vl;
        va_start(vl, fmt);
        const int len = util::VSNPrintf(line, sizeof(line) - 1, fmt, vl);
        va_end(vl);
        if (len <= 0) {
            return;
        }
        const int n = std::min<int>(len, sizeof(line) - 2);
        line[n] = '\n';
        std::scoped_lock lk(g_log_lock);
        fs::FileHandle file;
        if (R_FAILED(fs::OpenFile(std::addressof(file), LogPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) {
            static_cast<void>(fs::CreateDirectory("sdmc:/lanywhere"));
            if (R_FAILED(fs::CreateFile(LogPath, 0)) || R_FAILED(fs::OpenFile(std::addressof(file), LogPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) {
                return;
            }
        }
        s64 size = 0;
        if (R_SUCCEEDED(fs::GetFileSize(std::addressof(size), file)) && size < 256 * 1024) {
            static_cast<void>(fs::WriteFile(file, size, line, n + 1, fs::WriteOption::Flush));
        }
        fs::CloseFile(file);
    }

    bool PublicEnabled() {
        std::scoped_lock lk(g_lock);
        LoadServer();
        return g_public;
    }

    void Publish(const void *network_info, size_t size) {
        if (InRoom()) {
            SendFrame(T_Publish, network_info, size);
        }
    }

    int FindGroups(u64 local_communication_id, void (*found)(const char *code, const void *network_info, void *user), void *user) {
        constexpr size_t InfoSize = 0x480, Record = CodeLength + InfoSize;
        static u8 reply[1 + 8 * Record];
        static constinit os::SdkMutex find_lock;
        std::scoped_lock flk(find_lock);
        {
            std::scoped_lock lk(g_lock);
            LoadServer();
        }
        const char *err = nullptr;
        const int fd = ConnectServer(std::addressof(err));
        if (fd < 0) {
            Log("buscar grupos: %s", err);
            return -1;
        }
        u8 body[8];
        put64(body, local_communication_id);
        u8 type = 0;
        ssize_t n = -1;
        if (SendFrameTo(fd, T_Find, body, sizeof(body), nullptr, 0)) {
            n = ReadFrame(fd, std::addressof(type), reply, sizeof(reply), ConnectTimeoutMs);
        }
        ::close(fd);
        if (n < 1 || type != T_Groups) {
            Log("buscar grupos: sin respuesta");
            return -1;
        }
        int count = 0;
        for (size_t i = 0; i < reply[0] && 1 + (i + 1) * Record <= static_cast<size_t>(n); i++, count++) {
            char code[CodeLength + 1] = {};
            std::memcpy(code, reply + 1 + i * Record, CodeLength);
            found(code, reply + 1 + i * Record + CodeLength, user);
        }
        Log("buscar grupos del juego %016lx: %d", local_communication_id, count);
        return count;
    }

    bool SendControl(u8 dest, LdnMsg msg, const void *data, size_t size) {
        if (!InRoom()) {
            return false;
        }
        const u8 head[2] = { dest, static_cast<u8>(msg) };
        Log("ldn -> miembro %u: mensaje %u (%zu bytes)", dest, static_cast<u8>(msg), size);
        return SendFrame(T_Relay, head, sizeof(head), data, size);
    }

    ssize_t TakeControl(u8 *from, LdnMsg *msg, void *buffer, size_t size) {
        std::scoped_lock lk(g_queue_lock);
        if (g_control_count == 0) {
            return -1;
        }
        const Control &c = g_controls[g_control_head];
        g_control_head = (g_control_head + 1) % ControlQueueSize;
        g_control_count--;
        *from = c.from;
        *msg = c.msg;
        const size_t n = std::min<size_t>(c.size, size);
        std::memcpy(buffer, c.data, n);
        return static_cast<ssize_t>(c.size);
    }

    ssize_t ReceiveData(u16 port, void *buffer, size_t size, u32 *src_ip, u16 *src_port) {
        std::scoped_lock lk(g_queue_lock);
        for (size_t i = 0; i < g_queue_count; i++) {
            Packet &p = g_queue[(g_queue_head + i) % QueueSize];
            if (p.dst_port != port) continue;
            const size_t n = std::min<size_t>(p.size, size);
            std::memcpy(buffer, p.data, n);
            *src_ip = p.src_ip;
            *src_port = p.src_port;
            // Quitar el paquete i manteniendo el orden del resto.
            for (size_t j = i; j + 1 < g_queue_count; j++) {
                g_queue[(g_queue_head + j) % QueueSize] = g_queue[(g_queue_head + j + 1) % QueueSize];
            }
            g_queue_count--;
            return static_cast<ssize_t>(n);
        }
        return -1;
    }

}
