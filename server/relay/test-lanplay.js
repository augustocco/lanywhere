'use strict';
// Pruebas de la pasarela LAN Play contra un servidor local: un "emulador" que habla LAN Play (ldn_mitm sobre IPv4)
// y una "consola" que habla el protocolo de salas, en los dos sentidos.
//   node test-lanplay.js

const assert = require('assert');
const net = require('net');
const dgram = require('dgram');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const L = require('./lanplay');

const PORT = 22460, LP_PORT = 22451;
const MAGIC = 0x594e414c;
const LDN = { SCAN: 1, SCAN_RESP: 2, CONNECT: 3, SYNC: 4, DISCONNECT: 5 };
const NI = 0x480, NODES = 0x68, NS = 0x40;
const GAME = 0x0100000000abc000n;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const ipStr = (ip) => [ip >>> 24, (ip >>> 16) & 255, (ip >>> 8) & 255, ip & 255].join('.');
async function until(fn, ms = 4000, what = 'condición') {
    const end = Date.now() + ms;
    while (Date.now() < end) { const v = fn(); if (v) return v; await sleep(20); }
    throw new Error(`no se cumplió: ${what}`);
}

function networkInfo(hostIp, name, players = 1) {
    const b = Buffer.alloc(NI);
    b.writeBigUInt64LE(GAME, 0);
    b[0x20] = 2; b.writeUInt32LE(hostIp, 0x22);
    b[0x66] = 8; b[0x67] = players;
    b.writeUInt32LE(hostIp, NODES); b[NODES + 11] = 1; b.write(name, NODES + 12);
    return b;
}
function nodeInfo(ip, name) {
    const n = Buffer.alloc(NS);
    n.writeUInt32LE(ip, 0); n[11] = 1; n.write(name, 12);
    return n;
}

// --- "Consola": protocolo de salas (TCP + UDP) ---
class Console {
    constructor(name) { this.name = name; this.frames = []; this.data = []; }
    async connect() {
        this.tcp = net.connect(PORT, '127.0.0.1');
        await new Promise((ok) => this.tcp.once('connect', ok));
        let p = Buffer.alloc(0);
        this.tcp.on('data', (c) => {
            p = Buffer.concat([p, c]);
            while (p.length >= 3 && p.length >= 2 + p.readUInt16LE(0)) {
                const len = p.readUInt16LE(0);
                this.frames.push({ type: p[2], body: p.subarray(3, 2 + len) });
                p = p.subarray(2 + len);
            }
        });
        this.udp = dgram.createSocket('udp4');
        this.udp.on('message', (m) => { if (m[4] === 0x82) this.data.push(m); });
        await new Promise((ok) => this.udp.bind(0, ok));
    }
    send(type, body = Buffer.alloc(0)) {
        const h = Buffer.alloc(3); h.writeUInt16LE(body.length + 1, 0); h[2] = type;
        this.tcp.write(Buffer.concat([h, body]));
    }
    async frame(type, pred = () => true, ms = 4000) {
        return until(() => { const i = this.frames.findIndex((f) => f.type === type && pred(f.body)); return i >= 0 && this.frames.splice(i, 1)[0].body; }, ms, `${this.name}: trama 0x${type.toString(16)}`);
    }
    onRoom(b) { this.code = b.subarray(0, 6).toString(); this.index = b[6]; this.ip = b.readUInt32LE(7); this.token = b.readUInt32LE(11); }
    async register() {
        const h = Buffer.alloc(9); h.writeUInt32LE(MAGIC, 0); h[4] = 1; h.writeUInt32LE(this.token, 5);
        this.udp.send(h, PORT, '127.0.0.1');
        await sleep(100);
    }
    sendData(dst, sport, dport, text) {
        const h = Buffer.alloc(17); h.writeUInt32LE(MAGIC, 0); h[4] = 2; h.writeUInt32LE(this.token, 5);
        h.writeUInt32LE(dst, 9); h.writeUInt16LE(sport, 13); h.writeUInt16LE(dport, 15);
        this.udp.send(Buffer.concat([h, Buffer.from(text)]), PORT, '127.0.0.1');
    }
    close() { this.tcp.destroy(); this.udp.close(); }
}

// --- "Emulador": LAN Play, ldn_mitm sobre IPv4 con el TCP de lanplay.js ---
class Emu {
    constructor(ip, name) { this.ip = ip; this.name = name; this.udpIn = []; this.tcps = new Map(); this.scanResults = []; this.hosting = null; }
    async start() {
        this.sock = dgram.createSocket('udp4');
        this.sock.on('message', (m) => this.onMsg(m));
        await new Promise((ok) => this.sock.bind(0, ok));
        this.raw(L.P.KEEPALIVE, Buffer.alloc(0));
    }
    raw(type, payload) { this.sock.send(Buffer.concat([Buffer.from([type]), payload]), LP_PORT, '127.0.0.1'); }
    sendIp(pkt) { this.raw(L.P.IPV4, pkt); }
    udp(dst, sport, dport, data) { this.sendIp(L.udpPacket(this.ip, sport, dst, dport, data)); }
    onMsg(m) {
        if (m[0] !== L.P.IPV4) return;
        const p = L.parseIpv4(m.subarray(1));
        if (!p) return;
        if (p.proto === 17) {
            const sport = p.body.readUInt16BE(0), dport = p.body.readUInt16BE(2), data = p.body.subarray(8);
            if (dport === 11452) {
                for (const f of L.ldnRead({}, data)) {
                    if (f.type === L.LT.SCAN_RESP) this.scanResults.push({ from: p.src, info: f.data });
                    if (f.type === L.LT.SCAN && this.hosting) this.udp(p.src, 11452, sport, L.ldnFrame(L.LT.SCAN_RESP, this.hosting));
                }
            } else {
                this.udpIn.push({ src: p.src, dst: p.dst, sport, dport, text: data.toString() });
            }
        } else if (p.proto === 6) {
            const t = p.body, sport = t.readUInt16BE(0), dport = t.readUInt16BE(2);
            const key = L.Tcp.key(this.ip, dport, p.src, sport);
            let c = this.tcps.get(key);
            if (!c && this.hosting && dport === 11452 && (t[13] & 2)) {
                c = new L.Tcp(this.ip, 11452, p.src, sport, (pkt) => this.sendIp(pkt));
                this.tcps.set(key, c);
                this.onStation(c);
                c.accept(t.readUInt32BE(4));
                return;
            }
            if (c) c.input(t);
        }
    }
    // Estación: unirse al grupo de host por TCP.
    join(hostIp, sport = 50001) {
        const c = new L.Tcp(this.ip, sport, hostIp, 11452, (pkt) => this.sendIp(pkt));
        this.tcps.set(c.key(), c);
        const st = {};
        this.syncs = [];
        c.onOpen = () => c.write(L.ldnFrame(L.LT.CONNECT, nodeInfo(this.ip, this.name)));
        c.onData = (d) => { for (const f of L.ldnRead(st, d)) if (f.type === L.LT.SYNC) this.syncs.push(f.data); };
        c.onClose = () => { this.closed = true; };
        c.connect();
        return c;
    }
    // Anfitrión: grupo con este emulador como nodo 0.
    host() {
        this.hosting = networkInfo(this.ip, this.name);
        this.stations = [];
    }
    onStation(c) {
        const st = {};
        c.onData = (d) => {
            for (const f of L.ldnRead(st, d)) {
                if (f.type !== L.LT.CONNECT) continue;
                const n = f.data;
                this.stations.push(n.readUInt32LE(0));
                this.hosting[0x67]++;
                const slot = NODES + this.hosting[0x67 - 0] * 0;
                n.copy(this.hosting, NODES + (this.hosting[0x67] - 1) * NS);
                this.hosting[NODES + (this.hosting[0x67] - 1) * NS + 10] = this.hosting[0x67] - 1;
                c.write(L.ldnFrame(L.LT.SYNC, this.hosting));
                void slot;
            }
        };
    }
    close() { this.sock.close(); }
}

async function main() {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lany-lp-'));
    const server = spawn(process.execPath, [path.join(__dirname, 'server.js'), '--port', String(PORT)], {
        stdio: ['ignore', 'pipe', 'inherit'],
        env: { ...process.env, LANYWHERE_STATE_DIR: dir, LANYWHERE_STATS_PORT: '0', LANYWHERE_PUBLIC_PORT: '0', LANYWHERE_LANPLAY_PORT: String(LP_PORT) },
    });
    const logs = [];
    server.stdout.on('data', (d) => logs.push(String(d)));
    await until(() => logs.join('').includes('lan play en UDP'), 5000, 'arranque');
    const ok = (m) => console.log('  ok', m);
    try {
        // ---------- A: grupo de una consola, un emulador se une ----------
        const host = new Console('Zorro Audaz 12');
        await host.connect();
        const g = Buffer.alloc(8); g.writeBigUInt64LE(GAME);
        const nick = Buffer.alloc(32); nick.write('Zorro Audaz 12');
        host.send(0x01, Buffer.concat([g, nick]));
        host.onRoom(await host.frame(0x81));
        await host.register();
        host.send(0x07, networkInfo(host.ip, 'Zorro Audaz 12'));  // PUBLISH
        await sleep(100);

        const emu = new Emu(0x0a0d0505, 'ryu-uno');
        await emu.start();
        emu.udp(L.BCAST, 11452, 11452, L.ldnFrame(L.LT.SCAN));
        const res = await until(() => emu.scanResults[0], 3000, 'respuesta a la búsqueda');
        const host13 = res.from;
        assert.strictEqual(res.info.readUInt32LE(NODES), host13, 'el nodo 0 lleva la IP 10.13 del anfitrión');
        assert.strictEqual(res.info.subarray(NODES + 12, NODES + 26).toString(), 'Zorro Audaz 12');
        ok(`el emulador ve el grupo de la consola (anfitrión en ${ipStr(host13)})`);

        emu.join(host13);
        const relayIn = await host.frame(0x84, (b) => b[1] === LDN.CONNECT);
        const stationIndex = relayIn[0];
        const node = relayIn.subarray(2);
        assert.strictEqual(node.readUInt32LE(0) >>> 24, 10, 'NodeInfo con IP de la sala');
        assert.strictEqual(node.readUInt32LE(0) & 0xffffff00, (10 << 24 | 114 << 16) >>> 0);
        ok(`la unión llega a la consola como un miembro más (miembro ${stationIndex})`);

        // La consola sincroniza el grupo: el emulador lo recibe con sus IPs 10.13.
        const sync = networkInfo(host.ip, 'Zorro Audaz 12', 2);
        node.copy(sync, NODES + NS); sync[NODES + NS + 10] = 1;
        host.send(0x04, Buffer.concat([Buffer.from([stationIndex, LDN.SYNC]), sync]));
        const got = await until(() => emu.syncs && emu.syncs[0], 3000, 'SyncNetwork en el emulador');
        assert.strictEqual(got.readUInt32LE(NODES), host13);
        assert.strictEqual(got.readUInt32LE(NODES + NS), emu.ip, 'la estación aparece con su propia IP 10.13');
        ok('el emulador recibe la sincronización con las IPs traducidas');

        // Juego: emulador → consola y consola → emulador.
        emu.udp(host13, 12345, 12345, Buffer.from('hola consola'));
        const d1 = await until(() => host.data.shift(), 3000, 'datos en la consola');
        assert.strictEqual(d1.subarray(13).toString(), 'hola consola');
        assert.strictEqual(d1.readUInt32LE(5), node.readUInt32LE(0), 'origen = IP de sala del emulador');
        host.sendData(node.readUInt32LE(0), 12345, 12345, 'hola emulador');
        const d2 = await until(() => emu.udpIn.shift(), 3000, 'datos en el emulador');
        assert.ok(d2.text === 'hola emulador' && d2.src === host13 && d2.dst === emu.ip);
        ok('los paquetes del juego van y vuelven traducidos');

        // ---------- B: grupo de un emulador, una consola se une ----------
        const emu2 = new Emu(0x0a0d0909, 'ryu-dos');
        await emu2.start();
        emu2.host();
        const finder = new Console('Lobo Feliz 33');
        await finder.connect();
        const lc = Buffer.alloc(8); lc.writeBigUInt64LE(GAME);
        const groups = await until(async () => null, 10, 'x').catch(() => null);
        void groups;
        let found = null;
        for (let i = 0; i < 40 && !found; i++) {
            finder.send(0x08, lc);
            const gr = await finder.frame(0x87);
            for (let k = 0; k < gr[0]; k++) {
                const rec = gr.subarray(1 + k * (6 + NI), 1 + (k + 1) * (6 + NI));
                if (rec.subarray(6 + NODES + 12, 6 + NODES + 19).toString() === 'ryu-dos') found = rec;
            }
            if (!found) await sleep(250);
        }
        assert.ok(found, 'el grupo del emulador aparece en FIND');
        const code = found.subarray(0, 6).toString();
        ok(`el grupo del emulador se publica como grupo abierto (sala ${code})`);

        const nick2 = Buffer.alloc(32); nick2.write('Lobo Feliz 33');
        finder.send(0x02, Buffer.concat([Buffer.from(code), nick2, lc]));
        finder.onRoom(await finder.frame(0x81));
        await finder.register();
        finder.send(0x04, Buffer.concat([Buffer.from([0, LDN.CONNECT]), nodeInfo(finder.ip, 'Lobo Feliz 33')]));
        const s2 = await finder.frame(0x84, (b) => b[1] === LDN.SYNC, 5000);
        const info2 = s2.subarray(2);
        assert.strictEqual(info2[0x67], 2);
        assert.strictEqual(info2.readUInt32LE(NODES + NS), finder.ip, 'la consola aparece con su IP de sala');
        const hostRoomIp = info2.readUInt32LE(NODES);
        assert.strictEqual(hostRoomIp & 0xffffff00, (10 << 24 | 114 << 16) >>> 0);
        assert.strictEqual(emu2.stations.length, 1);
        ok('la consola se une al grupo del emulador (TCP abierto por la pasarela, sincronización traducida)');

        finder.sendData(hostRoomIp, 12345, 12345, 'ping desde consola');
        const d3 = await until(() => emu2.udpIn.find((x) => x.text === 'ping desde consola'), 3000, 'datos en el emulador anfitrión');
        assert.strictEqual(d3.src, emu2.stations[0]);
        emu2.udp(d3.src, 12345, 12345, Buffer.from('pong desde emulador'));
        const d4 = await until(() => finder.data.find((m) => m.subarray(13).toString() === 'pong desde emulador'), 3000, 'datos en la consola');
        assert.strictEqual(d4.readUInt32LE(5), hostRoomIp);
        ok('paquetes del juego entre la consola y el emulador anfitrión');

        // ---------- C: entre emuladores, reenvío normal de LAN Play ----------
        emu.udp(emu2.ip, 7000, 7000, Buffer.from('entre emuladores'));
        await until(() => emu2.udpIn.find((x) => x.text === 'entre emuladores'), 3000, 'reenvío entre emuladores');
        ok('reenvío normal entre emuladores');

        // ---------- D: ping a una IP de la pasarela ----------
        const icmp = Buffer.alloc(16); icmp[0] = 8; icmp.writeUInt16BE(0x5279, 4);
        let sum = 0; for (let i = 0; i < 16; i += 2) sum += icmp.readUInt16BE(i);
        while (sum > 0xffff) sum = (sum & 0xffff) + (sum >>> 16);
        icmp.writeUInt16BE((~sum) & 0xffff, 2);
        let echoed = false;
        emu.sock.on('message', (m) => { const p = L.parseIpv4(m.subarray(1)); if (p && p.proto === 1 && p.body[0] === 0) echoed = true; });
        emu.sendIp(L.ipv4(emu.ip, host13, 1, icmp));
        await until(() => echoed, 2000, 'eco ICMP');
        ok('responde al ping en sus IPs (Ryujinx no las reutiliza)');

        for (const x of [host, finder]) x.close();
        emu.close(); emu2.close();
        console.log('TODAS LAS PRUEBAS DE LAN PLAY OK');
    } finally {
        server.kill();
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

main().then(() => process.exit(0), (e) => { console.error('FALLO:', e.message); process.exit(1); });
