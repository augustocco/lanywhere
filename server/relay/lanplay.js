'use strict';
// Pasarela LAN Play: deja que un emulador con modo "LAN Play" (el protocolo de switch-lan-play) entre en la red de
// LANywhere.
//
// Por dentro de LAN Play viaja el mismo modo local de ldn_mitm que usan las consolas, pero como paquetes IPv4 en
// una red virtual 10.13.0.0/16: búsqueda por broadcast UDP al puerto 11452, unión por TCP al 11452 del anfitrión y
// los paquetes del juego por UDP. Aquí:
//  - Se reenvía entre emuladores como un servidor de LAN Play normal (por la IP de destino).
//  - Los grupos abiertos de consolas aparecen al emulador que busca, y unirse lo mete en la sala como un miembro
//    virtual (con su IP 10.114.0.x de la sala).
//  - Los grupos que crea un emulador se publican como grupos abiertos de LANywhere, y las consolas que se unen
//    llegan al emulador por una conexión TCP que abre la pasarela en su nombre.
//  - Los paquetes del juego se traducen entre las direcciones 10.13.x.x y las 10.114.0.x de la sala.
//
// No hace falta cuenta ni contraseña: los emuladores usan los grupos abiertos.

const dgram = require('dgram');
const crypto = require('crypto');

// --- LAN Play -----------------------------------------------------------------------------------------------------
const P = { KEEPALIVE: 0x00, IPV4: 0x01, PING: 0x02, IPV4_FRAG: 0x03, AUTH_ME: 0x04, INFO: 0x10 };
const NET = 0x0a0d0000, MASK = 0xffff0000, BCAST = (NET | ~MASK) >>> 0;  // 10.13.0.0/16, 10.13.255.255
const GATEWAY_IP = (NET | 0xfefe) >>> 0;                                 // 10.13.254.254: la pasarela al buscar
const POOL_START = (NET | (240 << 8)) >>> 0, POOL_END = (NET | (253 << 8) | 254) >>> 0;  // IPs para las consolas
const CLIENT_IDLE_MS = 40000;
const MAX_CLIENTS_PER_IP = 8;
const LDN_PORT = 11452;
const SCAN_EVERY_MS = 3000;           // la pasarela pregunta a los emuladores si tienen grupo
const HOST_GONE_MS = 10000;           // sin respuesta a la búsqueda en este tiempo: el grupo se retira

// --- ldn_mitm (protocolo del modo local en LAN) --------------------------------------------------------------------
const LAN_MAGIC = 0x11451400;
const LT = { SCAN: 0, SCAN_RESP: 1, CONNECT: 2, SYNC: 3 };
const NETWORK_INFO_SIZE = 0x480, NODE_INFO_SIZE = 0x40, NODES = 0x68, NODE_SIZE = 0x40, MAX_NODES = 8;

// Mensajes de ldn entre miembros de una sala (LdnMsg en el sysmodule).
const LDN = { SCAN: 1, SCAN_RESP: 2, CONNECT: 3, SYNC: 4, DISCONNECT: 5 };
const T = { CREATE: 0x01, JOIN: 0x02, LEAVE: 0x03, RELAY: 0x04, PUBLISH: 0x07,
            ROOM: 0x81, MEMBER_JOINED: 0x82, MEMBER_LEFT: 0x83, RELAY_IN: 0x84, ERROR: 0x85 };
const ROOM_NET = (10 << 24) | (114 << 16);
const isRoomIp = (ip) => ((ip & 0xffffff00) >>> 0) === ROOM_NET;

const ipStr = (ip) => [ip >>> 24, (ip >>> 16) & 255, (ip >>> 8) & 255, ip & 255].join('.');

// --- IPv4 / UDP / TCP --------------------------------------------------------------------------------------------
function sum16(buf, acc = 0) {
    for (let i = 0; i + 1 < buf.length; i += 2) acc += buf.readUInt16BE(i);
    if (buf.length & 1) acc += buf[buf.length - 1] << 8;
    return acc;
}
function fold(acc) {
    while (acc > 0xffff) acc = (acc & 0xffff) + (acc >>> 16);
    return (~acc) & 0xffff;
}
function pseudo(src, dst, proto, len) {
    const b = Buffer.alloc(12);
    b.writeUInt32BE(src, 0); b.writeUInt32BE(dst, 4); b[9] = proto; b.writeUInt16BE(len, 10);
    return sum16(b);
}
let ipId = 1;
function ipv4(src, dst, proto, payload) {
    const h = Buffer.alloc(20);
    h[0] = 0x45; h.writeUInt16BE(20 + payload.length, 2); h.writeUInt16BE(ipId++ & 0xffff, 4);
    h[8] = 64; h[9] = proto; h.writeUInt32BE(src, 12); h.writeUInt32BE(dst, 16);
    h.writeUInt16BE(fold(sum16(h)), 10);
    return Buffer.concat([h, payload]);
}
function udpPacket(src, sport, dst, dport, data) {
    const u = Buffer.alloc(8 + data.length);
    u.writeUInt16BE(sport, 0); u.writeUInt16BE(dport, 2); u.writeUInt16BE(u.length, 4);
    data.copy(u, 8);
    u.writeUInt16BE(fold(sum16(u, pseudo(src, dst, 17, u.length))) || 0xffff, 6);
    return ipv4(src, dst, 17, u);
}
function parseIpv4(pkt) {
    if (pkt.length < 20 || (pkt[0] >> 4) !== 4) return null;
    const ihl = (pkt[0] & 15) * 4, total = pkt.readUInt16BE(2);
    if (ihl < 20 || total < ihl || total > pkt.length) return null;
    return { proto: pkt[9], src: pkt.readUInt32BE(12), dst: pkt.readUInt32BE(16), body: pkt.subarray(ihl, total) };
}

// --- ldn_mitm: tramas con cabecera de 12 bytes y compresión RLE de ceros ------------------------------------------
function ldnFrame(type, data = Buffer.alloc(0)) {
    const h = Buffer.alloc(12);
    h.writeUInt32LE(LAN_MAGIC, 0); h[4] = type; h[5] = 0; h.writeUInt16LE(data.length, 6);
    return Buffer.concat([h, data]);
}
function rleDecompress(input, expected) {
    const out = Buffer.alloc(expected);
    let o = 0;
    for (let i = 0; i < input.length && o < expected; ) {
        const c = input[i++];
        out[o++] = c;
        if (c === 0) {
            if (i >= input.length) return null;
            for (let n = input[i++]; n > 0 && o < expected; n--) out[o++] = 0;
        }
    }
    return o === expected ? out : null;
}
// Lee tramas de un flujo (TCP) o de un datagrama (UDP). Devuelve [{type, data}] y deja el resto en state.buf.
function ldnRead(state, chunk) {
    state.buf = state.buf ? Buffer.concat([state.buf, chunk]) : chunk;
    const out = [];
    while (state.buf.length >= 12) {
        if (state.buf.readUInt32LE(0) !== LAN_MAGIC) { state.buf = Buffer.alloc(0); break; }
        const len = state.buf.readUInt16LE(6);
        if (state.buf.length < 12 + len) break;
        let data = state.buf.subarray(12, 12 + len);
        if (state.buf[5]) data = rleDecompress(data, state.buf.readUInt16LE(8));
        const type = state.buf[4];
        state.buf = state.buf.subarray(12 + len);
        if (data) out.push({ type, data: Buffer.from(data) });
    }
    return out;
}

// --- TCP mínimo: lo justo para la conversación de unión de ldn_mitm (pocos mensajes pequeños) ---------------------
const TF = { FIN: 1, SYN: 2, RST: 4, PSH: 8, ACK: 16 };
const MSS = 1360, RTO_START = 300, RTO_MAX = 4000, MAX_RETRIES = 10;

class Tcp {
    // send(pkt): entrega un paquete IPv4 al otro lado. Eventos: onData(buf), onOpen(), onClose().
    constructor(local, lport, remote, rport, send) {
        Object.assign(this, { local, lport, remote, rport, send });
        this.state = 'closed';
        this.snd = crypto.randomBytes(4).readUInt32BE(0);  // siguiente número de secuencia a enviar
        this.una = this.snd;                               // el más antiguo sin confirmar
        this.rcv = 0;
        this.queue = [];                                   // segmentos enviados sin confirmar
        this.pending = [];                                 // datos que aún no caben (ventana)
        this.rto = RTO_START;
        this.timer = null;
        this.onData = this.onOpen = this.onClose = () => {};
    }
    key() { return Tcp.key(this.local, this.lport, this.remote, this.rport); }
    static key(a, ap, b, bp) { return `${a}:${ap}-${b}:${bp}`; }

    segment(flags, data = Buffer.alloc(0), seq = this.snd) {
        const t = Buffer.alloc(20 + data.length);
        t.writeUInt16BE(this.lport, 0); t.writeUInt16BE(this.rport, 2);
        t.writeUInt32BE(seq >>> 0, 4); t.writeUInt32BE(flags & TF.ACK ? this.rcv >>> 0 : 0, 8);
        t[12] = 5 << 4; t[13] = flags; t.writeUInt16BE(0xffff, 14);
        data.copy(t, 20);
        t.writeUInt16BE(fold(sum16(t, pseudo(this.local, this.remote, 6, t.length))), 16);
        this.send(ipv4(this.local, this.remote, 6, t));
    }
    // Envía con retransmisión hasta que se confirme (SYN y FIN ocupan un número de secuencia).
    reliable(flags, data = Buffer.alloc(0)) {
        const len = data.length + (flags & (TF.SYN | TF.FIN) ? 1 : 0);
        const seg = { seq: this.snd, flags, data, end: (this.snd + len) >>> 0 };
        this.snd = seg.end;
        this.queue.push(seg);
        this.segment(flags, data, seg.seq);
        this.arm();
    }
    arm() {
        if (this.timer || !this.queue.length) return;
        this.timer = setTimeout(() => {
            this.timer = null;
            if (!this.queue.length) return;
            if (++this.retries > MAX_RETRIES) return this.abort();
            this.rto = Math.min(this.rto * 2, RTO_MAX);
            for (const s of this.queue) this.segment(s.flags | (this.state === 'syn-sent' ? 0 : TF.ACK), s.data, s.seq);
            this.arm();
        }, this.rto);
        this.timer.unref?.();
        this.retries = this.retries || 0;
    }
    connect() {
        this.state = 'syn-sent';
        this.reliable(TF.SYN);
    }
    accept(seq) {
        this.rcv = (seq + 1) >>> 0;
        this.state = 'syn-received';
        this.reliable(TF.SYN | TF.ACK);
    }
    write(buf) {
        for (let i = 0; i < buf.length; i += MSS) this.reliable(TF.ACK | TF.PSH, buf.subarray(i, i + MSS));
    }
    close() {
        if (this.state === 'established' || this.state === 'close-wait') {
            this.state = this.state === 'close-wait' ? 'last-ack' : 'fin-wait';
            this.reliable(TF.FIN | TF.ACK);
        } else {
            this.abort();
        }
    }
    abort() {
        if (this.state === 'gone') return;
        if (this.state !== 'closed') this.segment(TF.RST | TF.ACK);
        this.finish();
    }
    finish() {
        clearTimeout(this.timer);
        this.timer = null;
        const was = this.state;
        this.state = 'gone';
        this.queue = [];
        if (was !== 'gone') this.onClose();
    }
    // Segmento recibido para esta conexión.
    input(t) {
        if (t.length < 20) return;
        const seq = t.readUInt32BE(4), ack = t.readUInt32BE(8), off = (t[12] >> 4) * 4, flags = t[13];
        const data = t.subarray(off);
        if (flags & TF.RST) return this.finish();
        if (this.state === 'syn-sent') {
            if ((flags & TF.SYN) && (flags & TF.ACK) && ack === this.snd) {
                this.rcv = (seq + 1) >>> 0;
                this.acked(ack);
                this.state = 'established';
                this.segment(TF.ACK);
                this.onOpen();
            }
            return;
        }
        if (flags & TF.SYN) {  // nuestro SYN-ACK se perdió y el otro repite el SYN
            if (this.state === 'syn-received') this.segment(TF.SYN | TF.ACK, Buffer.alloc(0), (this.snd - 1) >>> 0);
            return;
        }
        if (flags & TF.ACK) this.acked(ack);
        if (this.state === 'syn-received' && (flags & TF.ACK) && ack === this.snd) {
            this.state = 'established';
            this.onOpen();
        }
        let advanced = false;
        if (data.length) {
            if (seq === this.rcv) {
                this.rcv = (this.rcv + data.length) >>> 0;
                advanced = true;
                this.onData(Buffer.from(data));
            }
            this.segment(TF.ACK);  // confirmar (o volver a pedir lo que falta)
        }
        if ((flags & TF.FIN) && ((seq + data.length) >>> 0) === this.rcv) {
            this.rcv = (this.rcv + 1) >>> 0;
            this.segment(TF.ACK);
            if (this.state === 'established') {
                this.state = 'close-wait';
                this.close();  // cerramos también: no hay nada más que enviar
            } else if (this.state === 'fin-wait') {
                this.finish();
            }
        } else if (!advanced && !data.length && this.state === 'last-ack' && !this.queue.length) {
            this.finish();
        }
        if (this.state === 'fin-wait' && !this.queue.length && (flags & TF.FIN)) this.finish();
    }
    acked(ack) {
        const before = this.queue.length;
        this.queue = this.queue.filter((s) => ((ack - s.end) | 0) < 0);
        if (this.queue.length !== before) {
            this.retries = 0;
            this.rto = RTO_START;
            clearTimeout(this.timer);
            this.timer = null;
            this.arm();
        }
        if (this.state === 'last-ack' && !this.queue.length) this.finish();
    }
}

// --- La pasarela -------------------------------------------------------------------------------------------------
function create({ port, log, rooms, count, paused, frame, forwardData }) {
    if (!port) return null;
    const sock = dgram.createSocket('udp4');
    const clients = new Map();   // "ip:puerto" del emulador → { address, port, seen, ips:Set }
    const byIp = new Map();      // IP 10.13 → cliente (aprendida del origen de sus paquetes)
    const tcps = new Map();      // conexiones TCP de la pasarela
    const bridges = new Map();   // código de sala → puente (ver bridge())
    const owned = new Map();     // IP 10.13 que responde la pasarela → { bridge, member } (consolas)
    const perIp = new Map();
    let nextPool = POOL_START;

    function sendTo(client, type, payload) {
        if (!client) return;
        const b = Buffer.concat([Buffer.from([type]), payload]);
        sock.send(b, client.port, client.address);
        count(b.length + 28);
    }
    function deliverIpv4(dst, pkt, except) {
        if (dst === BCAST || dst === 0xffffffff) {
            for (const c of clients.values()) if (c !== except) sendTo(c, P.IPV4, pkt);
        } else {
            sendTo(byIp.get(dst), P.IPV4, pkt);
        }
    }

    // Una IP 10.13 libre para que una consola aparezca en la red de LAN Play.
    function allocIp() {
        for (let n = 0; n <= POOL_END - POOL_START; n++) {
            const ip = nextPool;
            nextPool = nextPool >= POOL_END ? POOL_START : nextPool + 1;
            if ((ip & 0xff) === 0 || (ip & 0xff) === 255 || owned.has(ip) || byIp.has(ip)) continue;
            return ip;
        }
        return 0;
    }

    // Miembro virtual: un participante del lado LAN Play (un emulador) dentro de una sala.
    function virtualMember(bridgeObj, publicAddress, ip13, onFrame) {
        const vs = {
            virtual: true, destroyed: false, remoteAddress: publicAddress, member: null, ip13,
            deliver: (type, body) => onFrame(vs, type, body),
            data: (from, dst, portsPayload) => {
                // Paquete de la sala para el emulador: de la IP 10.13 de quien lo envía a la del emulador.
                const src13 = bridgeObj.to13(from.ip);
                if (!src13 || !byIp.has(ip13)) return;
                const sport = portsPayload.readUInt16LE(0), dport = portsPayload.readUInt16LE(2);
                const to = dst === ((ROOM_NET | 255) >>> 0) || dst === 0xffffffff ? BCAST : ip13;
                sendTo(byIp.get(ip13), P.IPV4, udpPacket(src13, sport, to, dport, portsPayload.subarray(4)));
            },
        };
        return vs;
    }

    // Puente de una sala: traduce IPs entre la sala (10.114.0.x) y LAN Play (10.13.x.x).
    //  origin 'console': la sala la creó una consola; los emuladores entran como estaciones.
    //  origin 'emu': el grupo lo creó un emulador; la pasarela publica la sala y une a las consolas.
    function bridge(code, origin) {
        let b = bridges.get(code);
        if (b) return b;
        b = {
            code, origin,
            emu: new Map(),     // IP 10.13 de un emulador → socket virtual (con .member)
            consoles: new Map(), // índice de miembro consola → IP 10.13 asignada
            host13: 0, lastSeen: Date.now(), info: null,
            to13(ip114) {
                const room = rooms.get(code);
                if (!room) return 0;
                const m = room.members.find((x) => x && x.ip === ip114);
                if (!m) return 0;
                if (m.socket && m.socket.virtual) return m.socket.ip13;
                if (!b.consoles.has(m.index)) {
                    const ip = allocIp();
                    if (!ip) return 0;
                    b.consoles.set(m.index, ip);
                    owned.set(ip, { bridge: b, index: m.index });
                }
                return b.consoles.get(m.index);
            },
            to114(ip13) {
                const room = rooms.get(code);
                if (!room) return 0;
                const vs = b.emu.get(ip13);
                if (vs && vs.member) return vs.member.ip;
                const o = owned.get(ip13);
                if (o && o.bridge === b) {
                    const m = room.members[o.index];
                    return m ? m.ip : 0;
                }
                return 0;
            },
        };
        bridges.set(code, b);
        return b;
    }
    function dropBridge(b) {
        for (const [, vs] of b.emu) if (vs.member) frame(vs, T.LEAVE, Buffer.alloc(0));
        for (const [, ip] of b.consoles) owned.delete(ip);
        for (const t of [...tcps.values()]) if (t.bridge === b) t.abort();
        bridges.delete(b.code);
    }

    // NetworkInfo de la sala (10.114) → para LAN Play (10.13), y al revés. mapIp devuelve 0 si no sabe.
    function rewriteInfo(info, mapIp) {
        const out = Buffer.from(info);
        for (let i = 0; i < MAX_NODES; i++) {
            const n = NODES + i * NODE_SIZE;
            if (!out[n + 11]) continue;
            const ip = mapIp(out.readUInt32LE(n));
            if (ip) out.writeUInt32LE(ip >>> 0, n);
        }
        return out;
    }

    // ---------- Grupos de consolas → emuladores --------------------------------------------------------------
    // Un emulador busca grupo: respondemos con los grupos abiertos de consolas, como si sus anfitriones estuvieran
    // en la LAN.
    function answerScan(client, src, sport) {
        for (const room of rooms.values()) {
            const pub = room.published;
            if (!pub || pub.info[0x67] >= pub.info[0x66]) continue;
            const existing = bridges.get(room.code);
            if (existing && existing.origin === 'emu') continue;  // ese grupo ya lo ve por LAN Play
            if (pub.member.socket && pub.member.socket.virtual) continue;
            const b = bridge(room.code, 'console');
            const info = rewriteInfo(pub.info, (ip) => b.to13(ip) || ip);
            const host13 = b.to13(pub.member.ip);
            if (!host13) continue;
            sendTo(client, P.IPV4, udpPacket(host13, LDN_PORT, src, sport, ldnFrame(LT.SCAN_RESP, info)));
        }
    }

    // TCP de un emulador al 11452 de un anfitrión consola: es la unión al grupo.
    function acceptStation(client, ip, b, hostIndex, t) {
        const room = rooms.get(b.code);
        if (!room) return t.abort();
        const vs = virtualMember(b, client.address, ip, onStationFrame);
        let joined = false, nodeInfo = null;
        const st = {};
        t.bridge = b;
        function onStationFrame(_vs, type, body) {
            if (type === T.ROOM) {
                joined = true;
                b.emu.set(ip, vs);
                if (nodeInfo) sendConnect();
            } else if (type === T.ERROR) {
                t.close();
            } else if (type === T.RELAY_IN && body.length >= 2) {
                const from = body[0], msg = body[1], data = body.subarray(2);
                if (from !== hostIndex) return;
                if (msg === LDN.SYNC && data.length === NETWORK_INFO_SIZE) {
                    t.write(ldnFrame(LT.SYNC, rewriteInfo(data, (x) => b.to13(x))));
                } else if (msg === LDN.DISCONNECT) {
                    t.close();
                }
            } else if (type === T.MEMBER_LEFT && body[0] === hostIndex) {
                t.close();
            }
        }
        function sendConnect() {
            const node = Buffer.from(nodeInfo);
            node.writeUInt32LE(vs.member.ip, 0);
            frame(vs, T.RELAY, Buffer.concat([Buffer.from([hostIndex, LDN.CONNECT]), node]));
        }
        t.onData = (chunk) => {
            for (const f of ldnRead(st, chunk)) {
                if (f.type !== LT.CONNECT || f.data.length !== NODE_INFO_SIZE || nodeInfo) continue;
                nodeInfo = f.data;
                const name = nodeInfo.subarray(12, 45);
                const end = name.indexOf(0);
                const nick = Buffer.alloc(32);
                name.subarray(0, end < 0 ? 32 : Math.min(end, 32)).copy(nick);
                const gameId = Buffer.alloc(8);
                gameId.writeBigUInt64LE(room.gameId);
                frame(vs, T.JOIN, Buffer.concat([Buffer.from(b.code, 'ascii'), nick, gameId]));
                if (joined) sendConnect();
            }
        };
        t.onClose = () => {
            tcps.delete(t.key());
            if (vs.member) {
                frame(vs, T.RELAY, Buffer.from([hostIndex, LDN.DISCONNECT]));
                frame(vs, T.LEAVE, Buffer.alloc(0));
            }
            b.emu.delete(ip);
            log(`lan play: ${ipStr(ip)} sale del grupo de la sala ${b.code}`);
        };
        log(`lan play: ${ipStr(ip)} se une al grupo de la sala ${b.code}`);
    }

    // ---------- Grupos de emuladores → consolas --------------------------------------------------------------
    // Respuesta a nuestra búsqueda: un emulador tiene grupo. Se publica como grupo abierto de LANywhere.
    function onEmuGroup(client, host13, info) {
        if (paused()) return;
        let b = [...bridges.values()].find((x) => x.origin === 'emu' && x.host13 === host13);
        if (!b) {
            const node0 = NODES;
            const name = info.subarray(node0 + 12, node0 + 45);
            const end = name.indexOf(0);
            const nick = Buffer.alloc(32);
            name.subarray(0, end < 0 ? 32 : Math.min(end, 32)).copy(nick);
            const gameId = Buffer.alloc(8);
            gameId.writeBigUInt64LE(info.readBigUInt64LE(0));
            const host = { b: null };
            const vs = virtualMember({ to13: (ip) => (host.b ? host.b.to13(ip) : 0) }, client.address, host13,
                (_vs, type, body) => onHostFrame(host.b, _vs, type, body));
            frame(vs, T.CREATE, Buffer.concat([gameId, nick]));
            if (!vs.member) return;
            b = bridge(vs.member.room.code, 'emu');
            host.b = b;
            b.host13 = host13;
            b.emu.set(host13, vs);
            log(`lan play: grupo de ${ipStr(host13)} publicado en la sala ${b.code}`);
        }
        b.lastSeen = Date.now();
        b.info = info;
        publishEmu(b);
    }
    // Lo que publica (y ven las consolas): el NetworkInfo del emulador con las IPs de la sala.
    function publishEmu(b) {
        const vs = b.emu.get(b.host13);
        if (!vs || !vs.member || !b.info) return;
        // Las estaciones del grupo que sean emuladores sin miembro aún (entraron por LAN Play) se añaden a la sala.
        for (let i = 1; i < MAX_NODES; i++) {
            const n = NODES + i * NODE_SIZE;
            if (!b.info[n + 11]) continue;
            const ip13 = b.info.readUInt32LE(n);
            if (b.emu.has(ip13) || owned.has(ip13) || !byIp.has(ip13)) continue;
            addEmuStation(b, ip13, b.info.subarray(n + 12, n + 45));
        }
        frame(vs, T.PUBLISH, rewriteInfo(b.info, (ip) => b.to114(ip)));
    }
    function addEmuStation(b, ip13, rawName) {
        const vs = virtualMember(b, byIp.get(ip13).address, ip13, () => {});
        const end = rawName.indexOf(0);
        const nick = Buffer.alloc(32);
        rawName.subarray(0, end < 0 ? 32 : Math.min(end, 32)).copy(nick);
        const gameId = Buffer.alloc(8);
        gameId.writeBigUInt64LE(rooms.get(b.code).gameId);
        frame(vs, T.JOIN, Buffer.concat([Buffer.from(b.code, 'ascii'), nick, gameId]));
        if (vs.member) b.emu.set(ip13, vs);
    }
    // Lo que le llega al miembro virtual anfitrión (el emulador): consolas que se unen a su grupo.
    function onHostFrame(b, vs, type, body) {
        if (!b) return;
        if (type === T.RELAY_IN && body.length >= 2) {
            const from = body[0], msg = body[1], data = body.subarray(2);
            if (msg === LDN.CONNECT && data.length === NODE_INFO_SIZE) connectConsole(b, from, data);
            else if (msg === LDN.DISCONNECT) closeConsole(b, from);
            else if (msg === LDN.SCAN && b.info)  // una consola dentro de la sala busca: el grupo es este
                frame(vs, T.RELAY, Buffer.concat([Buffer.from([from, LDN.SCAN_RESP]), rewriteInfo(b.info, (ip) => b.to114(ip))]));
        } else if (type === T.MEMBER_LEFT) {
            closeConsole(b, body[0]);
        }
    }
    // Una consola se une al grupo de un emulador: la pasarela abre el TCP al anfitrión en su nombre.
    function connectConsole(b, index, nodeInfo) {
        const room = rooms.get(b.code);
        const m = room && room.members[index];
        if (!m || (m.socket && m.socket.virtual)) return;
        closeConsole(b, index);
        const ip13 = b.to13(m.ip);
        if (!ip13) return;
        const sport = 40000 + (crypto.randomBytes(2).readUInt16BE(0) % 20000);
        const t = new Tcp(ip13, sport, b.host13, LDN_PORT, (pkt) => deliverIpv4(b.host13, pkt));
        t.bridge = b;
        t.consoleIndex = index;
        tcps.set(t.key(), t);
        const st = {};
        t.onOpen = () => {
            const node = Buffer.from(nodeInfo);
            node.writeUInt32LE(ip13 >>> 0, 0);
            t.write(ldnFrame(LT.CONNECT, node));
        };
        t.onData = (chunk) => {
            for (const f of ldnRead(st, chunk)) {
                if (f.type !== LT.SYNC || f.data.length !== NETWORK_INFO_SIZE) continue;
                b.info = f.data;
                b.lastSeen = Date.now();
                publishEmu(b);
                const vsHost = b.emu.get(b.host13);
                if (vsHost && vsHost.member)
                    frame(vsHost, T.RELAY, Buffer.concat([Buffer.from([index, LDN.SYNC]), rewriteInfo(f.data, (ip) => b.to114(ip))]));
            }
        };
        t.onClose = () => {
            tcps.delete(t.key());
            const vsHost = b.emu.get(b.host13);
            const still = rooms.get(b.code) && rooms.get(b.code).members[index];
            if (vsHost && vsHost.member && still && !t.quiet) frame(vsHost, T.RELAY, Buffer.from([index, LDN.DISCONNECT]));
        };
        t.connect();
        log(`lan play: consola ${index} de la sala ${b.code} entra en el grupo de ${ipStr(b.host13)}`);
    }
    function closeConsole(b, index) {
        for (const t of [...tcps.values()]) {
            if (t.bridge === b && t.consoleIndex === index) { t.quiet = true; t.close(); }
        }
    }

    // ---------- Paquetes de los emuladores ---------------------------------------------------------------------
    function onIpv4(client, pkt) {
        const p = parseIpv4(pkt);
        if (!p) return;
        if ((p.src & MASK) >>> 0 === NET && p.src !== BCAST) {
            if (byIp.get(p.src) !== client) {
                if (byIp.has(p.src) && Date.now() - byIp.get(p.src).seen < CLIENT_IDLE_MS && byIp.get(p.src) !== client) return;
                byIp.set(p.src, client);
                client.ips.add(p.src);
            }
        }
        const toGateway = p.dst === GATEWAY_IP || owned.has(p.dst);
        // ICMP eco a una IP de la pasarela (Ryujinx comprueba así que su IP no esté ocupada).
        if (p.proto === 1 && toGateway && p.body.length >= 8 && p.body[0] === 8) {
            const reply = Buffer.from(p.body);
            reply[0] = 0; reply.writeUInt16BE(0, 2);
            reply.writeUInt16BE(fold(sum16(reply)), 2);
            return sendTo(client, P.IPV4, ipv4(p.dst, p.src, 1, reply));
        }
        if (p.proto === 17 && p.body.length >= 8) {
            const sport = p.body.readUInt16BE(0), dport = p.body.readUInt16BE(2);
            const data = p.body.subarray(8, p.body.readUInt16BE(4));
            if (dport === LDN_PORT) {
                for (const f of ldnRead({}, data)) {
                    if (f.type === LT.SCAN && (p.dst === BCAST || p.dst === 0xffffffff)) answerScan(client, p.src, sport);
                    else if (f.type === LT.SCAN_RESP && p.dst === GATEWAY_IP && f.data.length === NETWORK_INFO_SIZE)
                        onEmuGroup(client, p.src, f.data);
                }
                if (p.dst === GATEWAY_IP || owned.has(p.dst)) return;
            } else {
                gameData(p.src, p.dst, sport, dport, data);
                if (owned.has(p.dst)) return;
            }
        }
        if (p.proto === 6 && toGateway) return onTcp(client, p);
        deliverIpv4(p.dst, pkt, client);  // reenvío normal entre emuladores
    }
    // Un emulador envía un paquete del juego: si va a alguien de una sala (o a todos), se traduce.
    function gameData(src13, dst13, sport, dport, data) {
        for (const b of bridges.values()) {
            const vs = b.emu.get(src13);
            if (!vs || !vs.member) continue;
            const ports = Buffer.alloc(4);
            ports.writeUInt16LE(sport, 0); ports.writeUInt16LE(dport, 2);
            const pp = Buffer.concat([ports, data]);
            if (dst13 === BCAST || dst13 === 0xffffffff) forwardData(vs.member, (ROOM_NET | 255) >>> 0, pp);
            else {
                const o = owned.get(dst13);
                if (o && o.bridge === b) {
                    const m = rooms.get(b.code) && rooms.get(b.code).members[o.index];
                    if (m) forwardData(vs.member, m.ip, pp);
                }
            }
        }
    }
    function onTcp(client, p) {
        const t = p.body;
        if (t.length < 20) return;
        const sport = t.readUInt16BE(0), dport = t.readUInt16BE(2), flags = t[13];
        const key = Tcp.key(p.dst, dport, p.src, sport);
        let conn = tcps.get(key);
        if (!conn) {
            const o = owned.get(p.dst);
            if (!(flags & TF.SYN) || (flags & TF.ACK) || dport !== LDN_PORT || !o || o.bridge.origin !== 'console') {
                if (!(flags & TF.RST)) {  // nadie escucha: RST
                    const r = new Tcp(p.dst, dport, p.src, sport, (pkt) => sendTo(client, P.IPV4, pkt));
                    r.rcv = (t.readUInt32BE(4) + Math.max(1, t.length - (t[12] >> 4) * 4)) >>> 0;
                    r.state = 'x';
                    r.segment(TF.RST | TF.ACK, Buffer.alloc(0), (flags & TF.ACK) ? t.readUInt32BE(8) : 0);
                }
                return;
            }
            const room = rooms.get(o.bridge.code);
            if (!room || !room.published || room.published.member.index !== o.index) return;
            conn = new Tcp(p.dst, dport, p.src, sport, (pkt) => sendTo(byIp.get(p.src) || client, P.IPV4, pkt));
            tcps.set(key, conn);
            acceptStation(client, p.src, o.bridge, o.index, conn);
            conn.accept(t.readUInt32BE(4));
            return;
        }
        conn.input(t);
    }

    // ---------- Socket LAN Play --------------------------------------------------------------------------------
    sock.on('message', (msg, rinfo) => {
        if (!msg.length) return;
        const key = `${rinfo.address}:${rinfo.port}`;
        let client = clients.get(key);
        if (!client) {
            const n = perIp.get(rinfo.address) || 0;
            if (n >= MAX_CLIENTS_PER_IP) return;
            perIp.set(rinfo.address, n + 1);
            client = { address: rinfo.address, port: rinfo.port, seen: 0, ips: new Set() };
            clients.set(key, client);
            log(`lan play: entra un emulador (${clients.size} conectados)`);
        }
        client.seen = Date.now();
        count(msg.length + 28);
        const type = msg[0] & 0x7f, payload = msg.subarray(1);
        if (type === P.IPV4) onIpv4(client, payload);
        else if (type === P.PING) sendTo(client, P.PING, payload);
        else if (type === P.IPV4_FRAG && payload.length >= 16) {
            // Fragmentos: se reenvían por su destino sin reensamblar (la pasarela no los necesita).
            const dst = payload.readUInt32BE(4);
            if (!owned.has(dst) && dst !== GATEWAY_IP) {
                if (dst === BCAST) for (const c of clients.values()) { if (c !== client) sendTo(c, P.IPV4_FRAG, payload); }
                else sendTo(byIp.get(dst), P.IPV4_FRAG, payload);
            }
        }
    });
    sock.on('error', (e) => log(`lan play: ${e.message}`));
    sock.bind(port, () => log(`lan play en UDP ${port}`));

    // Mantenimiento: buscar grupos de emuladores, retirar los que ya no responden, olvidar clientes inactivos.
    const scan = ldnFrame(LT.SCAN);
    const timer = setInterval(() => {
        const now = Date.now();
        for (const [key, c] of clients) {
            if (now - c.seen > CLIENT_IDLE_MS) {
                clients.delete(key);
                perIp.set(c.address, Math.max(0, (perIp.get(c.address) || 1) - 1));
                for (const ip of c.ips) if (byIp.get(ip) === c) byIp.delete(ip);
                log(`lan play: sale un emulador (${clients.size} conectados)`);
            }
        }
        for (const b of [...bridges.values()]) {
            const room = rooms.get(b.code);
            if (!room) { dropBridge(b); continue; }
            if (b.origin === 'emu' && now - b.lastSeen > HOST_GONE_MS) {
                log(`lan play: el grupo de ${ipStr(b.host13)} ya no responde; se retira la sala ${b.code}`);
                dropBridge(b);
            } else if (b.origin === 'console' && !b.emu.size && !room.published) {
                dropBridge(b);
            }
        }
        if (!clients.size || paused()) return;
        for (const c of clients.values()) sendTo(c, P.IPV4, udpPacket(GATEWAY_IP, LDN_PORT, BCAST, LDN_PORT, scan));
    }, SCAN_EVERY_MS);
    timer.unref();

    return {
        stats: () => ({ clients: clients.size, bridges: bridges.size }),
        close: () => { clearInterval(timer); sock.close(); },
    };
}

module.exports = { create, Tcp, ldnFrame, ldnRead, udpPacket, parseIpv4, ipv4, P, LT, NET, BCAST, GATEWAY_IP };
