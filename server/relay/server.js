'use strict';
// Servidor de salas de LANywhere: control por TCP y paquetes del juego por UDP (ver docs/PROTOCOL.md).
// Sin dependencias: node server.js [--port 11460]
//
// Variables de entorno:
//   LANYWHERE_CAP_GB      tráfico de salida máximo al mes (GB; por defecto 8000). Al
//                    llegar, el servidor se pone en pausa hasta el mes siguiente (UTC).
//   LANYWHERE_STATE_DIR   dónde guardar el contador de tráfico y la historia del panel (por defecto, la carpeta del
//                    servidor).
//   LANYWHERE_STATS_PORT  puerto del panel de actividad, solo en 127.0.0.1 (por defecto 11461; 0 lo desactiva).
//   LANYWHERE_PUBLIC_PORT puerto de los datos públicos para la web, solo en 127.0.0.1 (por defecto 11463; 0 lo
//                    desactiva). Lo publica el proxy HTTPS.

const fs = require('fs');
const path = require('path');
const net = require('net');
const dgram = require('dgram');
const crypto = require('crypto');

const PORT = Number(process.argv[process.argv.indexOf('--port') + 1]) || 11460;
const MAX_MEMBERS = 8;
const MAX_PAYLOAD = 1400;
const IDLE_MS = 20000;
const GRACE_MS = 60000;  // puesto guardado si se corta la conexión (al cambiar de red, la consola reconecta)
const UDP_MAGIC = 0x594e414c;
const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const NET_BASE = (10 << 24) | (114 << 16);  // 10.114.0.0/24
// Grupo de ldn publicado (NetworkInfo de ldn_mitm): id del juego en 0, máximo de jugadores en 0x66, jugadores en
// 0x67, 8 nodos de 0x40 desde 0x68 (IP en +0, conectado en +11, nombre de 33 bytes en +12).
const NETWORK_INFO_SIZE = 0x480;
const NODES = 0x68, NODE_SIZE = 0x40;
const MAX_GROUPS = 6;  // por respuesta a FIND
const BROADCASTS = new Set([(NET_BASE | 255) >>> 0, 0xffffffff]);

// Límites contra abusos (por IP pública: una casa con varias consolas comparte IP).
const LIMITS = {
    connectionsPerIp: 12,
    roomsTotal: 3000,
    perMinute: { create: 10, join: 30, find: 120 },
};
const CAP_BYTES = (Number(process.env.LANYWHERE_CAP_GB) || 8000) * 1e9;
const STATE_FILE = path.join(process.env.LANYWHERE_STATE_DIR || __dirname, 'traffic.json');
const STATS_PORT = process.env.LANYWHERE_STATS_PORT === undefined ? 11461 : Number(process.env.LANYWHERE_STATS_PORT);
const PUBLIC_PORT = process.env.LANYWHERE_PUBLIC_PORT === undefined ? 11463 : Number(process.env.LANYWHERE_PUBLIC_PORT);
const UDP_OVERHEAD = 28, TCP_OVERHEAD = 52;  // cabeceras IP/UDP e IP/TCP: también cuentan como tráfico

const T = { CREATE: 0x01, JOIN: 0x02, LEAVE: 0x03, RELAY: 0x04, PING: 0x05, RESUME: 0x06, PUBLISH: 0x07, FIND: 0x08,
            ROOM: 0x81, MEMBER_JOINED: 0x82, MEMBER_LEFT: 0x83, RELAY_IN: 0x84, ERROR: 0x85, PONG: 0x86, GROUPS: 0x87 };
const U = { REGISTER: 0x01, DATA: 0x02, KEEPALIVE: 0x03, REGISTERED: 0x81, DATA_IN: 0x82 };
const E = { NO_ROOM: 1, FULL: 2, OTHER_GAME: 3, BAD_REQUEST: 4, NO_SEAT: 5, PAUSED: 6, BUSY: 7 };
const LDN_SCAN_RESP = 2, LDN_SYNC = 4;

const rooms = new Map();    // código → { code, gameId, members: Array(8), published }
const byToken = new Map();  // token → miembro

// Panel de actividad (stats.js): hasta que arranca, no cuenta nada.
let stats = { count() {}, event() {}, noteSearch() {}, save() {} };
const log = (...a) => {
    console.log(new Date().toISOString(), ...a);
    stats.event(a.join(' '));
};
const ipOf = (index) => (NET_BASE | (index + 1)) >>> 0;
const ipStr = (ip) => [ip >>> 24, (ip >>> 16) & 255, (ip >>> 8) & 255, ip & 255].join('.');
const plainIp = (a) => (a || '').replace(/^::ffff:/, '');

// --- apodos ---------------------------------------------------------------------------------------------------
// Los elige cada consola (nunca el nombre de usuario de la consola); aquí solo se limpian. Si un apodo no es válido
// o es ofensivo, el jugador aparece como "Jugador NNNN".

const BAD_WORDS = [
    // español
    'puta', 'puto', 'mierda', 'pendejo', 'pendeja', 'marica', 'maricon', 'verga', 'polla', 'coño', 'cono', 'culo',
    'chinga', 'chingada', 'cabron', 'cabrona', 'gonorrea', 'hijueputa', 'malparido', 'malparida', 'zorra', 'perra',
    'idiota', 'imbecil', 'estupido', 'estupida', 'mamaguevo', 'pajero', 'follar', 'violar', 'nazi', 'hitler',
    'sexo', 'porno', 'pene', 'vagina', 'tetas', 'negrata', 'sudaca', 'retrasado', 'mongolico', 'subnormal',
    // inglés
    'fuck', 'shit', 'bitch', 'cunt', 'dick', 'cock', 'pussy', 'whore', 'slut', 'nigger', 'nigga', 'faggot', 'fag',
    'retard', 'rape', 'porn', 'sex', 'penis', 'asshole', 'bastard',
];

function normalizeForFilter(s) {
    return s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '')
        .replace(/[0@4]/g, (c) => ({ 0: 'o', '@': 'a', 4: 'a' })[c])
        .replace(/[1!|]/g, 'i').replace(/3/g, 'e').replace(/5|\$/g, 's').replace(/7/g, 't')
        .replace(/[^a-zñ]/g, '');
}

function cleanNick(raw, fallbackSeed) {
    let s = String(raw || '').normalize('NFC').replace(/[^\p{L}\p{N} ._-]/gu, '').replace(/\s+/g, ' ').trim();
    if ([...s].length > 20) s = [...s].slice(0, 20).join('').trim();
    const flat = normalizeForFilter(s);
    const offensive = BAD_WORDS.some((w) => flat.includes(w));
    if ([...s].length < 2 || offensive) {
        stats.count('nicksFiltered');
        const n = 1000 + (fallbackSeed % 9000);
        return `Jugador ${n}`;
    }
    return s;
}

function nick(buf) {
    const end = buf.indexOf(0);
    return buf.subarray(0, end < 0 ? buf.length : end).toString('utf8');
}

function nickBuf(s) {
    const b = Buffer.alloc(32);
    b.write(s || '', 0, 31, 'utf8');
    return b;
}

// Los nombres de los nodos de un NetworkInfo se reemplazan por los apodos limpios de la sala: un cliente modificado
// no puede poner ahí otro nombre (y nunca se ve el nombre de usuario de la consola).
function sanitizeNetworkInfo(room, info) {
    const out = Buffer.from(info);
    for (let i = 0; i < 8; i++) {
        const node = NODES + i * NODE_SIZE;
        out.fill(0, node + 12, node + 45);
        if (!out[node + 11]) continue;
        const ip = out.readUInt32LE(node);
        const member = room.members.find((m) => m && m.ip === ip);
        out.write(member ? member.nick : 'Jugador', node + 12, 32, 'utf8');
    }
    return out;
}

// --- tráfico del mes: tope para no pasarse del presupuesto -------------------------------------------------------

const traffic = { month: '', bytes: 0 };
const monthNow = () => new Date().toISOString().slice(0, 7);
let paused = false;

function loadTraffic() {
    try {
        Object.assign(traffic, JSON.parse(fs.readFileSync(STATE_FILE, 'utf8')));
    } catch { /* primera vez */ }
    checkMonth();
}

function saveTraffic() {
    try {
        fs.writeFileSync(STATE_FILE, JSON.stringify(traffic));
    } catch (e) {
        log(`no se pudo guardar el tráfico: ${e.message}`);
    }
}

function checkMonth() {
    const m = monthNow();
    if (traffic.month !== m) {
        if (traffic.month) log(`mes nuevo (${m}): contador de tráfico a cero`);
        traffic.month = m;
        traffic.bytes = 0;
    }
    const wasPaused = paused;
    paused = traffic.bytes >= CAP_BYTES;
    if (paused && !wasPaused) {
        log(`TOPE MENSUAL ALCANZADO (${(traffic.bytes / 1e9).toFixed(1)} GB): servidor en pausa hasta el mes que viene`);
        for (const room of rooms.values())
            for (const m of room.members) if (m && m.socket) sendError(m.socket, E.PAUSED, PAUSED_TEXT);
    }
    if (!paused && wasPaused) log('servidor activo de nuevo');
}
const PAUSED_TEXT = 'el servidor llegó a su límite de este mes; vuelve el día 1';

function count(bytes) {
    traffic.bytes += bytes;
}

// --- límites por IP --------------------------------------------------------------------------------------------

const connections = new Map();  // ip → conexiones abiertas
const buckets = new Map();      // `${ip}:${tipo}` → { minute, n }

function allowed(ip, kind) {
    const minute = Math.floor(Date.now() / 60000);
    const key = `${ip}:${kind}`;
    let b = buckets.get(key);
    if (!b || b.minute !== minute) buckets.set(key, (b = { minute, n: 0 }));
    if (++b.n <= LIMITS.perMinute[kind]) return true;
    stats.count('rateLimited');
    return false;
}

setInterval(() => {
    const minute = Math.floor(Date.now() / 60000);
    for (const [k, b] of buckets) if (b.minute < minute) buckets.delete(k);
}, 60000).unref();

// --- salas -----------------------------------------------------------------------------------------------------

function newCode() {
    for (;;) {
        let code = '';
        for (const byte of crypto.randomBytes(6)) code += CODE_ALPHABET[byte % CODE_ALPHABET.length];
        if (!rooms.has(code)) return code;
    }
}

function newToken() {
    for (;;) {
        const t = crypto.randomBytes(4).readUInt32LE(0);
        if (t && !byToken.has(t)) return t;
    }
}

function memberBuf(m) {
    const b = Buffer.alloc(5);
    b.writeUInt8(m.index, 0);
    b.writeUInt32LE(m.ip, 1);
    return Buffer.concat([b, nickBuf(m.nick)]);
}

// Un "socket" puede ser una conexión TCP de una consola o un miembro virtual (un Ryujinx entrando por LAN Play, ver
// lanplay.js): ese recibe las tramas por deliver() en lugar de por la red.
function send(socket, type, body = Buffer.alloc(0)) {
    if (!socket || socket.destroyed) return;
    if (socket.virtual) return socket.deliver(type, body);
    const head = Buffer.alloc(3);
    head.writeUInt16LE(body.length + 1, 0);
    head.writeUInt8(type, 2);
    socket.write(Buffer.concat([head, body]));
    count(3 + body.length + TCP_OVERHEAD);
}

function sendError(socket, code, text) {
    send(socket, T.ERROR, Buffer.concat([Buffer.from([code]), Buffer.from(text, 'utf8')]));
}

function others(room, member) {
    return room.members.filter((m) => m && m !== member);
}

function join(socket, room, nickname) {
    const index = room.members.findIndex((m) => !m);
    if (index < 0) return sendError(socket, E.FULL, 'la sala está llena');
    const token = newToken();
    const member = { index, ip: ipOf(index), nick: cleanNick(nickname, token), token, socket, room, udp: null, grace: null,
                     publicIp: plainIp(socket.remoteAddress) };
    room.members[index] = member;
    byToken.set(member.token, member);
    socket.member = member;
    sendRoom(member);
    for (const m of others(room, member)) send(m.socket, T.MEMBER_JOINED, memberBuf(member));
    stats.count('joins');
    log(`sala ${room.code}: entra ${member.nick} (${ipStr(member.ip)}), ${room.members.filter(Boolean).length} miembros`);
}

function sendRoom(member) {
    const { room, index } = member;
    const head = Buffer.alloc(6 + 1 + 4 + 4 + 8 + 1);
    head.write(room.code, 0, 6, 'ascii');
    head.writeUInt8(index, 6);
    head.writeUInt32LE(member.ip, 7);
    head.writeUInt32LE(member.token, 11);
    head.writeBigUInt64LE(room.gameId, 15);
    const list = room.members.filter(Boolean);
    head.writeUInt8(list.length, 23);
    send(member.socket, T.ROOM, Buffer.concat([head, ...list.map(memberBuf)]));
}

// Conexión cortada sin LEAVE: el puesto (índice, IP y token) se guarda GRACE_MS por si vuelve con RESUME.
function detach(member) {
    if (member.socket) member.socket.member = null;
    member.socket = null;
    clearTimeout(member.grace);
    member.grace = setTimeout(() => leave(member, 'no volvió'), GRACE_MS);
    log(`sala ${member.room.code}: se cortó ${member.nick}, se guarda su puesto`);
}

function leave(member, why) {
    const { room } = member;
    if (!room || room.members[member.index] !== member) return;
    room.members[member.index] = null;
    byToken.delete(member.token);
    clearTimeout(member.grace);
    if (member.socket) member.socket.member = null;
    if (room.published && room.published.member === member) room.published = null;
    for (const m of others(room, member)) send(m.socket, T.MEMBER_LEFT, Buffer.from([member.index]));
    const left = room.members.filter(Boolean).length;
    log(`sala ${room.code}: sale ${member.nick} (${why}), quedan ${left}`);
    if (left === 0) rooms.delete(room.code);
}

function onFrame(socket, type, body) {
    const member = socket.member;
    const ip = plainIp(socket.remoteAddress);
    if (paused && type !== T.LEAVE && type !== T.PING) {
        stats.count('pausedRejects');
        return sendError(socket, E.PAUSED, PAUSED_TEXT);
    }
    switch (type) {
        case T.CREATE: {
            if (member || body.length < 8 + 32) return sendError(socket, E.BAD_REQUEST, 'CREATE no válido');
            if (!allowed(ip, 'create') || rooms.size >= LIMITS.roomsTotal)
                return sendError(socket, E.BUSY, 'demasiadas salas; prueba en un momento');
            const room = { code: newCode(), gameId: body.readBigUInt64LE(0), members: new Array(MAX_MEMBERS).fill(null), published: null };
            rooms.set(room.code, room);
            stats.count('roomsCreated');
            log(`sala ${room.code} creada (juego ${room.gameId.toString(16)})`);
            return join(socket, room, nick(body.subarray(8, 40)));
        }
        case T.JOIN: {
            if (member || body.length < 6 + 32) return sendError(socket, E.BAD_REQUEST, 'JOIN no válido');
            if (!allowed(ip, 'join')) return sendError(socket, E.BUSY, 'demasiados intentos; prueba en un momento');
            const room = rooms.get(body.subarray(0, 6).toString('ascii').toUpperCase());
            if (!room) return sendError(socket, E.NO_ROOM, 'la sala no existe');
            const gameId = body.length >= 6 + 32 + 8 ? body.readBigUInt64LE(38) : 0n;
            if (gameId && gameId !== room.gameId) return sendError(socket, E.OTHER_GAME, 'la sala es de otro juego');
            return join(socket, room, nick(body.subarray(6, 38)));
        }
        case T.RESUME: {
            if (member || body.length < 4 + 6) return sendError(socket, E.BAD_REQUEST, 'RESUME no válido');
            if (!allowed(ip, 'join')) return sendError(socket, E.BUSY, 'demasiados intentos; prueba en un momento');
            const old = byToken.get(body.readUInt32LE(0));
            if (!old || old.room.code !== body.subarray(4, 10).toString('ascii'))
                return sendError(socket, E.NO_SEAT, 'el puesto en la sala ya no existe');
            if (old.socket) { old.socket.member = null; old.socket.destroy(); }
            clearTimeout(old.grace);
            old.grace = null;
            old.socket = socket;
            old.publicIp = ip;  // al reconectar puede haber cambiado de red
            old.udp = null;     // y hay que volver a registrar el UDP desde ella
            socket.member = old;
            sendRoom(old);
            stats.count('resumes');
            log(`sala ${old.room.code}: vuelve ${old.nick} (${ipStr(old.ip)})`);
            return;
        }
        case T.LEAVE:
            if (member) leave(member, 'salió');
            return;
        case T.RELAY: {
            if (!member || body.length < 1) return;
            const dest = body.readUInt8(0);
            let payload = body.subarray(1);
            // Grupos de ldn entre miembros: con los apodos de la sala en los nombres de los nodos.
            if (payload.length === 1 + NETWORK_INFO_SIZE && (payload[0] === LDN_SCAN_RESP || payload[0] === LDN_SYNC))
                payload = Buffer.concat([payload.subarray(0, 1), sanitizeNetworkInfo(member.room, payload.subarray(1))]);
            const out = Buffer.concat([Buffer.from([member.index]), payload]);
            for (const m of others(member.room, member))
                if (dest === 0xff || m.index === dest) send(m.socket, T.RELAY_IN, out);
            return;
        }
        case T.PUBLISH: {
            // El anfitrión anuncia su grupo (o lo retira con cuerpo vacío): aparece en FIND para su juego.
            if (!member) return;
            if (body.length === 0) {
                if (member.room.published && member.room.published.member === member) member.room.published = null;
            } else if (body.length === NETWORK_INFO_SIZE) {
                const first = !member.room.published;
                member.room.published = { member, info: sanitizeNetworkInfo(member.room, body), lcid: body.readBigUInt64LE(0) };
                if (first) stats.count('groupsPublished');
                if (first) log(`sala ${member.room.code}: grupo publicado (juego ${member.room.published.lcid.toString(16)})`);
            }
            return;
        }
        case T.FIND: {
            if (!allowed(ip, 'find')) return send(socket, T.GROUPS, Buffer.from([0]));
            // Origen (byte 14): 1 = la lista de la app (se refresca cada pocos segundos), 0 = un juego buscando grupo.
            const fromApp = body.length >= 15 && body[14] === 1;
            if (!fromApp) stats.count('finds');
            stats.noteSearch(ip, fromApp ? 'app' : 'game');
            // Grupos abiertos de un juego (0: todos), con sitio libre; los que tienen más gente primero. Con un código
            // después del juego (el grupo elegido en la app), ese va el primero aunque haya muchos.
            const lcid = body.length >= 8 ? body.readBigUInt64LE(0) : 0n;
            const preferred = body.length >= 14 && body[8] ? body.subarray(8, 14).toString('ascii').toUpperCase() : '';
            const found = [...rooms.values()]
                .map((r) => r.published && { code: r.code, info: r.published.info, lcid: r.published.lcid })
                .filter((g) => g && (!lcid || g.lcid === lcid) && g.info[0x67] < g.info[0x66])
                .sort((a, b) => (b.code === preferred) - (a.code === preferred) || b.info[0x67] - a.info[0x67])
                .slice(0, MAX_GROUPS);
            const parts = [Buffer.from([found.length])];
            for (const g of found) parts.push(Buffer.from(g.code, 'ascii'), g.info);
            return send(socket, T.GROUPS, Buffer.concat(parts));
        }
        case T.PING:
            return send(socket, T.PONG);
        default:
            return sendError(socket, E.BAD_REQUEST, `tipo ${type} desconocido`);
    }
}

const tcp = net.createServer((socket) => {
    const ip = plainIp(socket.remoteAddress);
    const open = (connections.get(ip) || 0) + 1;
    if (open > LIMITS.connectionsPerIp) {
        socket.destroy();
        return;
    }
    connections.set(ip, open);
    socket.setNoDelay(true);
    socket.setTimeout(IDLE_MS);
    let pending = Buffer.alloc(0);
    socket.on('data', (chunk) => {
        pending = Buffer.concat([pending, chunk]);
        while (pending.length >= 3) {
            const len = pending.readUInt16LE(0);
            if (len < 1 || len > 4096) return socket.destroy();
            if (pending.length < 2 + len) break;
            const frame = pending.subarray(2, 2 + len);
            pending = pending.subarray(2 + len);
            onFrame(socket, frame.readUInt8(0), frame.subarray(1));
        }
    });
    socket.on('timeout', () => socket.destroy());
    socket.on('close', () => {
        const n = (connections.get(ip) || 1) - 1;
        if (n > 0) connections.set(ip, n); else connections.delete(ip);
        if (socket.member) detach(socket.member);
    });
    socket.on('error', () => {});
});

const udp = dgram.createSocket('udp4');
function udpSend(buf, port, address) {
    udp.send(buf, port, address);
    count(buf.length + UDP_OVERHEAD);
}

udp.on('message', (msg, rinfo) => {
    if (msg.length < 9 || msg.readUInt32LE(0) !== UDP_MAGIC) return;
    const type = msg.readUInt8(4);
    const member = byToken.get(msg.readUInt32LE(5));
    // Solo se acepta UDP desde la misma IP pública que la conexión de control: así nadie puede, con un token
    // adivinado o capturado, hacer que el servidor envíe tráfico a la dirección de otro (ataque de reflexión).
    if (!member || rinfo.address !== member.publicIp) return;
    member.udp = { address: rinfo.address, port: rinfo.port };  // el router puede haber cambiado el puerto

    if (type === U.REGISTER) {
        const reply = Buffer.alloc(5);
        reply.writeUInt32LE(UDP_MAGIC, 0);
        reply.writeUInt8(U.REGISTERED, 4);
        udpSend(reply, rinfo.port, rinfo.address);
    } else if (type === U.DATA && !paused && msg.length >= 17 && msg.length - 17 <= MAX_PAYLOAD) {
        forwardData(member, msg.readUInt32LE(9), msg.subarray(13));
    }
});

// Paquete del juego de un miembro hacia otro de su sala (o a todos si dst es broadcast). ports_payload: puerto
// origen (u16) · puerto destino (u16) · carga.
function forwardData(member, dst, portsPayload) {
    if (paused || !member.room || portsPayload.length < 4 || portsPayload.length - 4 > MAX_PAYLOAD) return;
    const out = Buffer.alloc(portsPayload.length + 9);
    out.writeUInt32LE(UDP_MAGIC, 0);
    out.writeUInt8(U.DATA_IN, 4);
    out.writeUInt32LE(member.ip, 5);
    portsPayload.copy(out, 9);
    for (const m of others(member.room, member)) {
        if (!(BROADCASTS.has(dst) || m.ip === dst)) continue;
        if (m.socket && m.socket.virtual) m.socket.data(member, dst, portsPayload);
        else if (m.udp) udpSend(out, m.udp.port, m.udp.address);
    }
}

loadTraffic();

// Lo que muestra el panel en cada momento: consolas conectadas (miembros de salas con su conexión abierta), salas,
// grupos abiertos y jugadores por juego.
function snapshot() {
    let online = 0, privateRooms = 0, groups = 0, players = 0;
    const byGame = new Map();
    for (const r of rooms.values()) {
        online += r.members.filter((m) => m && m.socket).length;
        if (r.gameId === 0n) privateRooms++;
        if (!r.published) continue;
        const n = r.published.info[0x67];
        groups++;
        players += n;
        const key = r.published.lcid.toString(16).padStart(16, '0');
        const g = byGame.get(key) || { game: key, groups: 0, players: 0 };
        g.groups++;
        g.players += n;
        byGame.set(key, g);
    }
    return { online, rooms: rooms.size, privateRooms, groups, players,
             byGame: [...byGame.values()].sort((a, b) => b.players - a.players) };
}
// Grupos abiertos para la web pública: lo mismo que la lista de la app, sin el código de la sala.
function publicGroups() {
    const out = [];
    for (const r of rooms.values()) {
        if (!r.published) continue;
        const info = r.published.info, players = info[0x67], max = info[0x66];
        if (players >= max) continue;
        const name = info.subarray(NODES + 12, NODES + 45);
        const end = name.indexOf(0);
        out.push({ game: r.published.lcid.toString(16).padStart(16, '0'), host: name.subarray(0, end < 0 ? name.length : end).toString('utf8'),
                   players, max });
    }
    return out.sort((a, b) => b.players - a.players).slice(0, 50);
}
require('./public').create({ stateDir: path.dirname(STATE_FILE), port: PUBLIC_PORT, snapshot, groups: publicGroups });
// Pasarela LAN Play (Ryujinx): miembros virtuales que usan las mismas salas.
const LANPLAY_PORT = process.env.LANYWHERE_LANPLAY_PORT === undefined ? 11451 : Number(process.env.LANYWHERE_LANPLAY_PORT);
const lanplay = require('./lanplay').create({
    port: LANPLAY_PORT, log, rooms, count, paused: () => paused,
    frame: (socket, type, body) => onFrame(socket, type, body),
    forwardData,
});
stats = require('./stats').create({ stateDir: path.dirname(STATE_FILE), port: STATS_PORT, snapshot, traffic,
                                    capBytes: CAP_BYTES, paused: () => paused });
setInterval(() => { checkMonth(); saveTraffic(); }, 60000).unref();
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => { saveTraffic(); stats.save(); process.exit(0); });

tcp.listen(PORT, () => log(`control TCP en ${PORT}; tráfico del mes ${(traffic.bytes / 1e9).toFixed(2)} GB de ${CAP_BYTES / 1e9} GB`));
udp.bind(PORT, () => log(`datos UDP en ${PORT}`));

module.exports = { rooms, cleanNick, lanplay };
