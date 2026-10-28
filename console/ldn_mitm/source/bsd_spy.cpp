/*
 * LANywhere: observación de los sockets de los juegos (ver bsd_spy.hpp).
 *
 * This program is free software; you can redistribute it and/or modify it under the terms and conditions of
 * the GNU General Public License, version 2, as published by the Free Software Foundation.
 */
#include "bsd_spy.hpp"
#include "lanywhere_relay.hpp"
#include <cstdio>
#include <cstring>
#include <arpa/inet.h>

namespace ams::mitm::bsd {

    namespace {

        constexpr const char LogPath[] = "sdmc:/lanywhere/bsd.log";
        constexpr TimeSpan SummaryInterval = TimeSpan::FromSeconds(5);
        constexpr size_t MaxSockets = 64, MaxDestinations = 8;

        struct Destination { u32 ip; u16 port; u32 packets; u64 bytes; };
        struct SocketStats {
            bool used;
            s32 fd;
            u64 program_id;
            s32 type, protocol;
            u16 bound_port;  // el de bind(): los paquetes de la sala para ese puerto son de este socket
            u32 peer_ip;     // connect() a otro miembro de la sala (UDP): send() va por el servidor
            u16 peer_port;
            u32 sent_packets;
            u64 sent_bytes;
            Destination dest[MaxDestinations];
            size_t dest_count;
        };

        constinit os::SdkMutex g_lock;
        SocketStats g_sockets[MaxSockets];
        os::Tick g_last_summary;
        u32 g_overflow;

        // Lecturas: cómo recibe el juego (recv / recvfrom / poll) y cuánto.
        struct RecvStats { u32 recv_calls, recv_data, recv_eagain, recvfrom_calls, recvfrom_data, recvfrom_eagain, poll_calls, poll_ready; u64 bytes; };
        RecvStats g_recv;

        // Registro de depuración de los sockets del juego: solo si existe sdmc:/lanywhere/debug.flag (se mira una
        // vez; escribir en la SD con cada operación de red es trabajo de más para una partida normal).
        bool DebugEnabled() {
            static int enabled = -1;
            if (enabled < 0) {
                fs::DirectoryEntryType type;
                enabled = R_SUCCEEDED(fs::GetEntryType(std::addressof(type), "sdmc:/lanywhere/debug.flag")) ? 1 : 0;
            }
            return enabled == 1;
        }

        // Escritura directa al archivo (append); con g_lock tomado.
        void WriteLine(const char *fmt, ...) {
            if (!DebugEnabled()) {
                return;
            }
            char line[256];
            std::va_list vl;
            va_start(vl, fmt);
            const int len = util::VSNPrintf(line, sizeof(line) - 1, fmt, vl);
            va_end(vl);
            if (len <= 0) {
                return;
            }
            line[std::min<int>(len, sizeof(line) - 2)] = '\n';

            fs::FileHandle file;
            if (R_FAILED(fs::OpenFile(std::addressof(file), LogPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) {
                static_cast<void>(fs::CreateDirectory("sdmc:/lanywhere"));  // puede existir ya
                if (R_FAILED(fs::CreateFile(LogPath, 0)) || R_FAILED(fs::OpenFile(std::addressof(file), LogPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) {
                    return;
                }
            }
            s64 size = 0;
            if (R_SUCCEEDED(fs::GetFileSize(std::addressof(size), file))) {
                static_cast<void>(fs::WriteFile(file, size, line, std::min<int>(len, sizeof(line) - 2) + 1, fs::WriteOption::Flush));
            }
            fs::CloseFile(file);
        }

        const char *FormatAddr(char *out, size_t size, const void *data, size_t len) {
            if (len < sizeof(sockaddr_in)) {
                util::SNPrintf(out, size, "(addr %zu bytes)", len);
                return out;
            }
            const sockaddr_in *a = static_cast<const sockaddr_in *>(data);
            const u32 ip = ntohl(a->sin_addr.s_addr);
            util::SNPrintf(out, size, "%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, ntohs(a->sin_port));
            return out;
        }

        SocketStats *Find(s32 fd, u64 program_id, bool create) {
            SocketStats *free_slot = nullptr;
            for (auto &s : g_sockets) {
                if (s.used && s.fd == fd && s.program_id == program_id) {
                    return std::addressof(s);
                }
                if (!s.used && free_slot == nullptr) {
                    free_slot = std::addressof(s);
                }
            }
            if (create && free_slot != nullptr) {
                std::memset(free_slot, 0, sizeof(*free_slot));
                free_slot->used = true;
                free_slot->fd = fd;
                free_slot->program_id = program_id;
            }
            return create ? free_slot : nullptr;
        }

        void Summary(const SocketStats &s, const char *why) {
            WriteLine("  fd %d (%s/%d) %s: %u paquetes, %llu bytes enviados a %zu destinos", s.fd,
                      s.type == 1 ? "TCP" : s.type == 2 ? "UDP" : "?", s.protocol, why, s.sent_packets,
                      static_cast<unsigned long long>(s.sent_bytes), s.dest_count);
            for (size_t i = 0; i < s.dest_count; i++) {
                const auto &d = s.dest[i];
                WriteLine("    -> %u.%u.%u.%u:%u  %u paquetes, %llu bytes", d.ip >> 24, (d.ip >> 16) & 0xFF, (d.ip >> 8) & 0xFF, d.ip & 0xFF,
                          d.port, d.packets, static_cast<unsigned long long>(d.bytes));
            }
        }

        void MaybeSummary() {
            const os::Tick now = os::GetSystemTick();
            if ((now - g_last_summary).ToTimeSpan() < SummaryInterval) {
                return;
            }
            g_last_summary = now;
            WriteLine("[%lld s] resumen", static_cast<long long>(now.ToTimeSpan().GetSeconds()));
            for (const auto &s : g_sockets) {
                if (s.used && s.sent_packets) {
                    Summary(s, "activo");
                }
            }
            if (g_overflow) {
                WriteLine("  (%u sockets sin registrar: tabla llena)", g_overflow);
            }
            const auto &r = g_recv;
            WriteLine("  lecturas: recv %u (%u con datos, %u EAGAIN), recvfrom %u (%u con datos, %u EAGAIN), poll %u (%u listos), %llu bytes",
                      r.recv_calls, r.recv_data, r.recv_eagain, r.recvfrom_calls, r.recvfrom_data, r.recvfrom_eagain, r.poll_calls, r.poll_ready,
                      static_cast<unsigned long long>(r.bytes));
        }

        struct Out { s32 ret; s32 err; };
        constexpr s32 ErrnoAgain = 11;  // EAGAIN en el bsd de la consola
        struct PollFd { s32 fd; s16 events; s16 revents; };
        constexpr s16 PollIn = 0x0001, PollRdNorm = 0x0040;
        constexpr s32 PollSliceMs = 5;
        constexpr size_t MaxPollFds = 64;

        ssize_t TakeRoomPacket(s32 fd, u64 program_id, void *buffer, size_t size, sockaddr_in *from);

    }

    Result BsdSpyService::Poll(sf::Out<s32> out_ret, sf::Out<s32> out_errno, u32 nfds, s32 timeout, const sf::InAutoSelectBuffer &fds_in,
                               const sf::OutAutoSelectBuffer &fds_out) {
        // Los paquetes de la sala no pasan por el socket real, así que el poll de la consola no despierta con ellos:
        // si el juego espera datos en un socket de la sala, se espera a trozos cortos mirando también la cola.
        const size_t n = std::min<size_t>({ nfds, fds_in.GetSize() / sizeof(PollFd), fds_out.GetSize() / sizeof(PollFd), MaxPollFds });
        const PollFd *in_fds = reinterpret_cast<const PollFd *>(fds_in.GetPointer());
        u16 ports[MaxPollFds] = {};
        bool room = false;
        if (lanywhere::InRoom()) {
            std::scoped_lock lk(g_lock);
            for (size_t i = 0; i < n; i++) {
                if (!(in_fds[i].events & (PollIn | PollRdNorm))) continue;
                if (auto *s = Find(in_fds[i].fd, m_client_info.program_id.value, false); s != nullptr && s->bound_port) {
                    ports[i] = s->bound_port;
                    room = true;
                }
            }
        }

        const os::Tick start = os::GetSystemTick();
        Out out = {};
        for (;;) {
            s32 slice = timeout;
            if (room && timeout != 0) {
                const s64 elapsed = (os::GetSystemTick() - start).ToTimeSpan().GetMilliSeconds();
                slice = timeout < 0 ? PollSliceMs : static_cast<s32>(std::clamp<s64>(timeout - elapsed, 0, PollSliceMs));
            }
            const struct { u32 nfds; s32 timeout; } in = { nfds, slice };
            R_TRY(serviceDispatchInOut(m_forward_service.get(), 6, in, out,
                .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In, SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
                .buffers = { { fds_in.GetPointer(), fds_in.GetSize() }, { fds_out.GetPointer(), fds_out.GetSize() } },
            ));
            if (!room || out.ret < 0) break;
            PollFd *out_fds = reinterpret_cast<PollFd *>(fds_out.GetPointer());
            s32 ready = 0;
            for (size_t i = 0; i < n; i++) {
                if (ports[i] && lanywhere::HasData(ports[i])) {
                    out_fds[i].revents |= in_fds[i].events & (PollIn | PollRdNorm);
                }
                if (out_fds[i].revents) ready++;
            }
            out.ret = ready;
            if (ready > 0 || timeout == 0 || (timeout > 0 && (os::GetSystemTick() - start).ToTimeSpan().GetMilliSeconds() >= timeout)) break;
        }
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);

        std::scoped_lock lk(g_lock);
        g_recv.poll_calls++;
        if (out.ret > 0) {
            g_recv.poll_ready++;
        }
        MaybeSummary();
        R_SUCCEED();
    }

    Result BsdSpyService::Recv(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, s32 flags, const sf::OutAutoSelectBuffer &data) {
        const ssize_t room = TakeRoomPacket(fd, m_client_info.program_id.value, data.GetPointer(), data.GetSize(), nullptr);
        if (room >= 0) {
            out_ret.SetValue(static_cast<s32>(room));
            out_errno.SetValue(0);
            R_SUCCEED();
        }

        const struct { s32 fd, flags; } in = { fd, flags };
        Out out = {};
        R_TRY(serviceDispatchInOut(m_forward_service.get(), 8, in, out,
            .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
            .buffers = { { data.GetPointer(), data.GetSize() } },
        ));
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);

        std::scoped_lock lk(g_lock);
        g_recv.recv_calls++;
        if (out.ret > 0) {
            g_recv.recv_data++;
            g_recv.bytes += out.ret;
        } else if (out.err == ErrnoAgain) {
            g_recv.recv_eagain++;
        }
        MaybeSummary();
        R_SUCCEED();
    }

    namespace {

        // Paquete de la sala para el puerto del socket, si lo hay: tamaño entregado o -1.
        ssize_t TakeRoomPacket(s32 fd, u64 program_id, void *buffer, size_t size, sockaddr_in *from) {
            if (!lanywhere::InRoom()) {
                return -1;
            }
            u16 port = 0;
            {
                std::scoped_lock lk(g_lock);
                if (auto *s = Find(fd, program_id, false)) {
                    port = s->bound_port;
                }
            }
            if (port == 0) {
                return -1;
            }
            u32 src_ip;
            u16 src_port;
            const ssize_t n = lanywhere::ReceiveData(port, buffer, size, std::addressof(src_ip), std::addressof(src_port));
            if (n >= 0 && from != nullptr) {
                std::memset(from, 0, sizeof(*from));
                from->sin_len = sizeof(*from);
                from->sin_family = AF_INET;
                from->sin_port = htons(src_port);
                from->sin_addr.s_addr = htonl(src_ip);
            }
            return n;
        }

    }

    Result BsdSpyService::RecvFrom(sf::Out<s32> out_ret, sf::Out<s32> out_errno, sf::Out<u32> out_addrlen, s32 fd, s32 flags,
                                   const sf::OutAutoSelectBuffer &data, const sf::OutAutoSelectBuffer &addr) {
        sockaddr_in from;
        const ssize_t room = TakeRoomPacket(fd, m_client_info.program_id.value, data.GetPointer(), data.GetSize(), std::addressof(from));
        if (room >= 0) {
            const size_t addr_size = std::min(addr.GetSize(), sizeof(from));
            if (addr_size) {
                std::memcpy(addr.GetPointer(), std::addressof(from), addr_size);
            }
            out_ret.SetValue(static_cast<s32>(room));
            out_errno.SetValue(0);
            out_addrlen.SetValue(static_cast<u32>(addr_size));
            std::scoped_lock lk(g_lock);
            g_recv.recvfrom_calls++;
            g_recv.recvfrom_data++;
            g_recv.bytes += room;
            R_SUCCEED();
        }

        const struct { s32 fd, flags; } in = { fd, flags };
        struct { s32 ret; s32 err; u32 addrlen; } out = {};
        R_TRY(serviceDispatchInOut(m_forward_service.get(), 9, in, out,
            .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out, SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
            .buffers = { { data.GetPointer(), data.GetSize() }, { addr.GetPointer(), addr.GetSize() } },
        ));
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);
        out_addrlen.SetValue(out.addrlen);

        std::scoped_lock lk(g_lock);
        g_recv.recvfrom_calls++;
        if (out.ret > 0) {
            g_recv.recvfrom_data++;
            g_recv.bytes += out.ret;
        } else if (out.err == ErrnoAgain) {
            g_recv.recvfrom_eagain++;
        }
        MaybeSummary();
        R_SUCCEED();
    }

    Result BsdSpyService::Fcntl(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, s32 cmd, s32 arg) {
        const struct { s32 fd, cmd, arg; } in = { fd, cmd, arg };
        Out out = {};
        R_TRY(serviceDispatchInOut(m_forward_service.get(), 20, in, out));
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);

        std::scoped_lock lk(g_lock);
        WriteLine("[%016lx] fcntl(fd %d, cmd %d, arg 0x%x) = %d (errno %d)", m_client_info.program_id.value, fd, cmd, arg, out.ret, out.err);
        R_SUCCEED();
    }

    Result BsdSpyService::SetSockOpt(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, s32 level, s32 optname, const sf::InAutoSelectBuffer &optval) {
        const struct { s32 fd, level, optname; } in = { fd, level, optname };
        Out out = {};
        R_TRY(serviceDispatchInOut(m_forward_service.get(), 21, in, out,
            .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In },
            .buffers = { { optval.GetPointer(), optval.GetSize() } },
        ));
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);

        s32 value = 0;
        if (optval.GetSize() >= sizeof(s32)) {
            std::memcpy(std::addressof(value), optval.GetPointer(), sizeof(s32));
        }
        std::scoped_lock lk(g_lock);
        WriteLine("[%016lx] setsockopt(fd %d, nivel 0x%x, opción 0x%x, valor %d, %zu bytes) = %d (errno %d)", m_client_info.program_id.value, fd, level,
                  optname, value, optval.GetSize(), out.ret, out.err);
        R_SUCCEED();
    }

    Result BsdSpyService::Socket(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 domain, s32 type, s32 protocol) {
        const struct { s32 domain, type, protocol; } in = { domain, type, protocol };
        Out out = {};
        R_TRY(serviceDispatchInOut(m_forward_service.get(), 2, in, out));
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);

        std::scoped_lock lk(g_lock);
        WriteLine("[%016lx] socket(dom %d, tipo %d, proto %d) = %d (errno %d)", m_client_info.program_id.value, domain, type, protocol, out.ret, out.err);
        if (out.ret >= 0) {
            if (auto *s = Find(out.ret, m_client_info.program_id.value, true)) {
                s->type = type & 0xFF;
                s->protocol = protocol;
            } else {
                g_overflow++;
            }
        }
        R_SUCCEED();
    }

    Result BsdSpyService::Send(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, s32 flags, const sf::InAutoSelectBuffer &data) {
        if (lanywhere::InRoom()) {
            u32 peer_ip = 0;
            u16 peer_port = 0, src_port = 0;
            {
                std::scoped_lock lk(g_lock);
                if (auto *s = Find(fd, m_client_info.program_id.value, false); s != nullptr && s->peer_ip) {
                    peer_ip = s->peer_ip;
                    peer_port = s->peer_port;
                    src_port = s->bound_port;
                    s->sent_packets++;
                    s->sent_bytes += data.GetSize();
                }
            }
            if (peer_ip) {
                lanywhere::SendData(peer_ip, src_port, peer_port, data.GetPointer(), data.GetSize());
                out_ret.SetValue(static_cast<s32>(data.GetSize()));
                out_errno.SetValue(0);
                R_SUCCEED();
            }
        }
        const struct { s32 fd, flags; } in = { fd, flags };
        Out out = {};
        R_TRY(serviceDispatchInOut(m_forward_service.get(), 10, in, out,
            .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In },
            .buffers = { { data.GetPointer(), data.GetSize() } },
        ));
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);

        std::scoped_lock lk(g_lock);
        WriteLine("[%016lx] send(fd %d, %zu bytes) = %d (errno %d)", m_client_info.program_id.value, fd, data.GetSize(), out.ret, out.err);
        MaybeSummary();
        R_SUCCEED();
    }

    Result BsdSpyService::SendTo(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, s32 flags, const sf::InAutoSelectBuffer &data,
                                 const sf::InAutoSelectBuffer &addr) {
        // En una sala, lo que va a la red virtual (otro miembro o broadcast) sale por el servidor.
        if (lanywhere::InRoom() && addr.GetSize() >= sizeof(sockaddr_in)) {
            const sockaddr_in *a = reinterpret_cast<const sockaddr_in *>(addr.GetPointer());
            const u32 dst = ntohl(a->sin_addr.s_addr);
            if (lanywhere::IsVirtual(dst)) {
                u16 src_port = 0;
                {
                    std::scoped_lock lk(g_lock);
                    if (auto *s = Find(fd, m_client_info.program_id.value, true)) {
                        src_port = s->bound_port;
                        s->sent_packets++;
                        s->sent_bytes += data.GetSize();
                    }
                }
                lanywhere::SendData(dst, src_port, ntohs(a->sin_port), data.GetPointer(), data.GetSize());
                out_ret.SetValue(static_cast<s32>(data.GetSize()));
                out_errno.SetValue(0);
                R_SUCCEED();
            }
        }

        const struct { s32 fd, flags; } in = { fd, flags };
        Out out = {};
        R_TRY(serviceDispatchInOut(m_forward_service.get(), 11, in, out,
            .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In, SfBufferAttr_HipcAutoSelect | SfBufferAttr_In },
            .buffers = { { data.GetPointer(), data.GetSize() }, { addr.GetPointer(), addr.GetSize() } },
        ));
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);

        std::scoped_lock lk(g_lock);
        if (auto *s = Find(fd, m_client_info.program_id.value, true)) {
            s->sent_packets++;
            s->sent_bytes += data.GetSize();
            if (addr.GetSize() >= sizeof(sockaddr_in)) {
                const sockaddr_in *a = reinterpret_cast<const sockaddr_in *>(addr.GetPointer());
                const u32 ip = ntohl(a->sin_addr.s_addr);
                const u16 port = ntohs(a->sin_port);
                size_t i = 0;
                while (i < s->dest_count && !(s->dest[i].ip == ip && s->dest[i].port == port)) {
                    i++;
                }
                if (i == s->dest_count && s->dest_count < MaxDestinations) {
                    s->dest[s->dest_count++] = { ip, port, 0, 0 };
                }
                if (i < s->dest_count) {
                    s->dest[i].packets++;
                    s->dest[i].bytes += data.GetSize();
                }
            }
        }
        MaybeSummary();
        R_SUCCEED();
    }

    Result BsdSpyService::Bind(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, const sf::InAutoSelectBuffer &addr) {
        // En una sala la IP del juego es virtual: no existe en la consola, así que se escucha en todas.
        sockaddr_in local = {};
        const void *bind_addr = addr.GetPointer();
        size_t bind_size = addr.GetSize();
        if (lanywhere::InRoom() && bind_size >= sizeof(sockaddr_in)) {
            std::memcpy(std::addressof(local), bind_addr, sizeof(local));
            if (lanywhere::IsVirtual(ntohl(local.sin_addr.s_addr))) {
                local.sin_addr.s_addr = 0;
                bind_addr = std::addressof(local);
                bind_size = sizeof(local);
            }
        }
        Out out = {};
        R_TRY(serviceDispatchInOut(m_forward_service.get(), 13, fd, out,
            .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In },
            .buffers = { { bind_addr, bind_size } },
        ));
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);

        char a[48];
        std::scoped_lock lk(g_lock);
        WriteLine("[%016lx] bind(fd %d, %s) = %d (errno %d)", m_client_info.program_id.value, fd, FormatAddr(a, sizeof(a), addr.GetPointer(), addr.GetSize()), out.ret, out.err);
        if (out.ret == 0 && addr.GetSize() >= sizeof(sockaddr_in)) {
            if (auto *s = Find(fd, m_client_info.program_id.value, true)) {
                s->bound_port = ntohs(reinterpret_cast<const sockaddr_in *>(addr.GetPointer())->sin_port);
            }
        }
        R_SUCCEED();
    }

    Result BsdSpyService::Connect(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, const sf::InAutoSelectBuffer &addr) {
        // UDP "conectado" a otro miembro de la sala: no hay ruta real; send() lo manda por el servidor.
        if (lanywhere::InRoom() && addr.GetSize() >= sizeof(sockaddr_in)) {
            const sockaddr_in *a = reinterpret_cast<const sockaddr_in *>(addr.GetPointer());
            const u32 dst = ntohl(a->sin_addr.s_addr);
            std::scoped_lock lk(g_lock);
            auto *s = Find(fd, m_client_info.program_id.value, false);
            if (s != nullptr && s->type == 2 && lanywhere::IsVirtual(dst)) {
                s->peer_ip = dst;
                s->peer_port = ntohs(a->sin_port);
                char text[48];
                WriteLine("[%016lx] connect(fd %d, %s) a la sala (puerto local %u)", m_client_info.program_id.value, fd,
                          FormatAddr(text, sizeof(text), addr.GetPointer(), addr.GetSize()), s->bound_port);
                out_ret.SetValue(0);
                out_errno.SetValue(0);
                R_SUCCEED();
            }
        }
        Out out = {};
        R_TRY(serviceDispatchInOut(m_forward_service.get(), 14, fd, out,
            .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In },
            .buffers = { { addr.GetPointer(), addr.GetSize() } },
        ));
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);

        char a[48];
        std::scoped_lock lk(g_lock);
        WriteLine("[%016lx] connect(fd %d, %s) = %d (errno %d)", m_client_info.program_id.value, fd, FormatAddr(a, sizeof(a), addr.GetPointer(), addr.GetSize()), out.ret, out.err);
        R_SUCCEED();
    }

    Result BsdSpyService::Listen(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd, s32 backlog) {
        const struct { s32 fd, backlog; } in = { fd, backlog };
        Out out = {};
        R_TRY(serviceDispatchInOut(m_forward_service.get(), 18, in, out));
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);

        std::scoped_lock lk(g_lock);
        WriteLine("[%016lx] listen(fd %d, %d) = %d (errno %d)", m_client_info.program_id.value, fd, backlog, out.ret, out.err);
        R_SUCCEED();
    }

    Result BsdSpyService::Close(sf::Out<s32> out_ret, sf::Out<s32> out_errno, s32 fd) {
        Out out = {};
        R_TRY(serviceDispatchInOut(m_forward_service.get(), 26, fd, out));
        out_ret.SetValue(out.ret);
        out_errno.SetValue(out.err);

        std::scoped_lock lk(g_lock);
        if (auto *s = Find(fd, m_client_info.program_id.value, false)) {
            if (s->sent_packets) {
                Summary(*s, "cerrado");
            }
            s->used = false;
        }
        R_SUCCEED();
    }

}
