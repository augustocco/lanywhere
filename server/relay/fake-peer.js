'use strict';
// "Jugador falso" para probar el reenvío con una sola consola: se une a una sala y responde a lo que le llega.
//   node fake-peer.js CODIGO [host] [puerto]          contesta "pong <lo recibido>" y envía "hola N" cada 2 s
//   node fake-peer.js CODIGO --ldn [host] [puerto]    hace de estación de ldn: busca el grupo que la consola creó
//                                                     en el juego, se une y muestra lo que sincroniza el anfitrión
//   node fake-peer.js --anfitrion [host] [puerto]     publica un grupo abierto falso: copia del último grupo real
//                                                     guardado por --abiertas (grupo.bin) o, si no hay, uno mínimo
//   node fake-peer.js --abiertas [host] [puerto]      igual, pero sin código: busca los grupos abiertos (FIND)
//                                                     y entra en la sala del primero

const fs = require('fs');
const path = require('path');
const net = require('net');
const dgram = require('dgram');

const args = process.argv.slice(2);
const OPEN = args.includes('--abiertas');
const HOSTING = args.includes('--anfitrion');
const LDN = OPEN || args.includes('--ldn');
const pos = args.filter((a) => !a.startsWith('--'));
let CODE = OPEN || HOSTING ? 'XXXXXX' : (pos[0] || '').toUpperCase();
const HOST = pos[OPEN || HOSTING ? 0 : 1] || '129.38.114.182';
const PORT = Number(pos[OPEN || HOSTING ? 1 : 2]) || 11460;
let foundInfo = null;
const MAGIC = 0x594e414c;
if (CODE.length !== 6) {
    console.error('uso: node fake-peer.js CODIGO [host] [puerto]');
    process.exit(2);
}

const ipStr = (ip) => [ip >>> 24, (ip >>> 16) & 255, (ip >>> 8) & 255, ip & 255].join('.');
let token = 0, myIp = 0, consoleIp = 0, consolePort = 5000, hello = 0, dataIn = 0;

// --- ldn por la sala (ver LdnMsg en console/ldn_mitm/source/lanywhere_relay.hpp) ---
const L = { SCAN: 1, SCAN_RESP: 2, CONNECT: 3, SYNC: 4, DISCONNECT: 5 };
const NETWORK_INFO_SIZE = 0x480, NODES = 0x68, NODE_SIZE = 0x40;
let host = -1, joined = false, hostedInfo = null;
function frame(type, body = Buffer.alloc(0)) {
    const h = Buffer.alloc(3);
    h.writeUInt16LE(body.length + 1, 0);
    h[2] = type;
    tcp.write(Buffer.concat([h, body]));
}
const relay = (dest, msg, data = Buffer.alloc(0)) => frame(0x04, Buffer.concat([Buffer.from([dest, msg]), data]));
function describe(info) {
    const lcid = info.readBigUInt64LE(0).toString(16).padStart(16, '0');
    const count = info[0x67], max = info[0x66];
    const names = [];
    for (let i = 0; i < 8; i++) {
        const n = info.subarray(NODES + i * NODE_SIZE);
        if (n[11]) names.push(`${n[10]}:${n.subarray(12, 45).toString().replace(/\0.*$/s, '')}@${ipStr(n.readUInt32LE(0))}`);
    }
    return `juego ${lcid}, ${count}/${max} jugadores: ${names.join(', ')}`;
}
function onLdn(from, msg, data) {
    if (msg === L.SCAN_RESP && data.length === NETWORK_INFO_SIZE) {
        console.log(`grupo encontrado (miembro ${from}): ${describe(data)}`);
        if (joined) return;
        joined = true;
        host = from;
        const node = Buffer.alloc(NODE_SIZE);
        node.writeUInt32LE(myIp, 0);
        node[4] = 0x02; node[5] = 0x00; node.writeUInt32LE(myIp, 6);
        node[11] = 1;
        node.write('pc-falso', 12);
        node.writeInt16LE(data.readInt16LE(NODES + 46), 46);  // misma versión de comunicación que el anfitrión
        relay(host, L.CONNECT, node);
        console.log('-> CONNECT enviado');
    } else if (msg === L.SYNC && data.length === NETWORK_INFO_SIZE) {
        console.log(`SYNC del anfitrión: ${describe(data)}`);
    } else if (msg === L.DISCONNECT) {
        console.log(`el miembro ${from} cerró el grupo o nos rechazó`);
        joined = false;
    } else if (msg === L.CONNECT && hostedInfo && data.length === NODE_SIZE) {
        // Somos anfitrión: la estación ocupa el nodo libre y se sincroniza el grupo con ella.
        let slot = -1;
        for (let i = 1; i < 8 && slot < 0; i++) if (!hostedInfo[NODES + i * NODE_SIZE + 11]) slot = i;
        if (slot < 0) return relay(from, L.DISCONNECT);
        data.copy(hostedInfo, NODES + slot * NODE_SIZE);
        hostedInfo[NODES + slot * NODE_SIZE + 10] = slot;
        hostedInfo[0x67]++;
        const name = data.subarray(12, 45).toString().replace(/\0.*$/s, '');
        console.log(`<- CONNECT de ${name} (miembro ${from}): nodo ${slot}; -> SYNC ${hostedInfo[0x67]}/${hostedInfo[0x66]}`);
        relay(from, L.SYNC, hostedInfo);
        frame(0x07, hostedInfo);
    } else if (msg === L.SCAN) {
        // somos estación: no tenemos grupo que anunciar
    } else {
        console.log(`ldn ${msg} de ${from} (${data.length} bytes)`);
    }
}

const udp = dgram.createSocket('udp4');
function udpSend(type, body = Buffer.alloc(0)) {
    const h = Buffer.alloc(9);
    h.writeUInt32LE(MAGIC, 0);
    h[4] = type;
    h.writeUInt32LE(token, 5);
    udp.send(Buffer.concat([h, body]), PORT, HOST);
}
function sendData(dst, srcPort, dstPort, text) {
    const b = Buffer.alloc(8);
    b.writeUInt32LE(dst, 0);
    b.writeUInt16LE(srcPort, 4);
    b.writeUInt16LE(dstPort, 6);
    udpSend(0x02, Buffer.concat([b, Buffer.from(text)]));
}
udp.on('message', (m) => {
    if (m.readUInt32LE(0) !== MAGIC) return;
    if (m[4] === 0x81) return console.log('UDP registrado');
    if (m[4] !== 0x82) return;
    const src = m.readUInt32LE(5), srcPort = m.readUInt16LE(9), dstPort = m.readUInt16LE(11);
    const text = m.subarray(13).toString();
    if (LDN) {
        if (++dataIn % 100 === 1) console.log(`datos del juego: ${dataIn} paquetes (último de ${ipStr(src)}:${srcPort} a ${dstPort}, ${m.length - 13} bytes)`);
        return;
    }
    console.log(`<- ${ipStr(src)}:${srcPort} -> puerto ${dstPort}: "${text}"`);
    consoleIp = src;
    consolePort = srcPort;
    sendData(src, dstPort, srcPort, `pong ${text}`);
});

const tcp = net.connect(PORT, HOST, () => {
    if (OPEN) return frame(0x08, Buffer.alloc(8));  // FIND de todos los juegos
    if (HOSTING) {
        const body = Buffer.alloc(8 + 32);
        body.writeBigUInt64LE(0x0100000000ABC000n, 0);
        body.write('pc-falso', 8);
        return frame(0x01, body);  // CREATE
    }
    sendJoin();
});
function sendJoin() {
    const nick = Buffer.alloc(32);
    nick.write('pc-falso');
    const body = Buffer.concat([Buffer.from(CODE, 'ascii'), nick]);
    const h = Buffer.alloc(3);
    h.writeUInt16LE(body.length + 1, 0);
    h[2] = 0x02;
    tcp.write(Buffer.concat([h, body]));
}
let pending = Buffer.alloc(0);
tcp.on('data', (c) => {
    pending = Buffer.concat([pending, c]);
    while (pending.length >= 3 && pending.length >= 2 + pending.readUInt16LE(0)) {
        const len = pending.readUInt16LE(0), type = pending[2], body = pending.subarray(3, 2 + len);
        pending = pending.subarray(2 + len);
        if (type === 0x87) {
            const n = body[0];
            console.log(`grupos abiertos: ${n}`);
            for (let i = 0; i < n; i++) {
                const rec = body.subarray(1 + i * (6 + NETWORK_INFO_SIZE), 1 + (i + 1) * (6 + NETWORK_INFO_SIZE));
                console.log(`  sala ${rec.subarray(0, 6)}: ${describe(rec.subarray(6))}`);
            }
            if (!n) process.exit(0);
            CODE = body.subarray(1, 7).toString('ascii');
            foundInfo = Buffer.from(body.subarray(7, 7 + NETWORK_INFO_SIZE));
            fs.writeFileSync(path.join(__dirname, 'grupo.bin'), foundInfo);
            console.log('   (guardado en grupo.bin para --anfitrion)');
            console.log(`-> entrando en la sala ${CODE}`);
            sendJoin();
            continue;
        }
        if (type === 0x85) {
            console.error(`error ${body[0]}: ${body.subarray(1).toString()}`);
            process.exit(1);
        }
        if (type === 0x81) {
            CODE = body.subarray(0, 6).toString('ascii');
            myIp = body.readUInt32LE(7);
            token = body.readUInt32LE(11);
            const n = body[23];
            for (let i = 0; i < n; i++) {
                const ip = body.readUInt32LE(24 + i * 37 + 1);
                if (ip !== myIp && !consoleIp) consoleIp = ip;
            }
            console.log(`en la sala ${CODE} como ${ipStr(myIp)}; consola: ${consoleIp ? ipStr(consoleIp) : '?'}`);
            udpSend(0x01);
            setInterval(() => udpSend(0x03), 5000);
            setInterval(() => {
                const h2 = Buffer.alloc(3);
                h2.writeUInt16LE(1, 0);
                h2[2] = 0x05;
                tcp.write(h2);
            }, 5000);
            if (HOSTING) {
                const saved = path.join(__dirname, 'grupo.bin');
                let info;
                if (fs.existsSync(saved)) {
                    // Grupo real: mismo juego, escena, anuncio y versión; se cambian anfitrión, BSSID y sesión.
                    info = fs.readFileSync(saved);
                    info.writeUInt32LE((0xc0ffee00 ^ myIp) >>> 0, 0x10);  // sessionId distinto
                    const version = info.readInt16LE(NODES + 46);
                    for (let i = 0; i < 8; i++) info.fill(0, NODES + i * NODE_SIZE, NODES + (i + 1) * NODE_SIZE);
                    for (let i = 0; i < 8; i++) info[NODES + i * NODE_SIZE + 10] = i;
                    info.writeInt16LE(version, NODES + 46);
                    console.log('usando el grupo real de grupo.bin');
                } else {
                    info = Buffer.alloc(NETWORK_INFO_SIZE);
                    info.writeBigUInt64LE(0x0100000000ABC000n, 0);
                    info[0x66] = 8;
                }
                info[0x20] = 0x02; info[0x21] = 0xfe; info.writeUInt32LE(myIp, 0x22);  // bssid propio
                info[0x67] = 1;
                info.writeUInt32LE(myIp, NODES);
                info[NODES + 4] = 0x02; info[NODES + 5] = 0xfe; info.writeUInt32LE(myIp, NODES + 6);
                info[NODES + 11] = 1;
                info.write('pc-falso', NODES + 12);
                hostedInfo = info;
                frame(0x07, info);  // PUBLISH
                console.log(`grupo falso publicado en la sala ${CODE} (Ctrl+C para retirarlo)`);
            } else if (OPEN) {
                onLdn(0, L.SCAN_RESP, foundInfo);  // el anfitrión es el miembro 0 de su sala
            } else if (LDN) {
                const scan = () => { if (!joined) { relay(0xff, L.SCAN); console.log('-> SCAN (busca grupos)'); } };
                scan();
                setInterval(scan, 3000);
            } else {
                setInterval(() => consoleIp && sendData(consoleIp, 5000, consolePort, `hola ${++hello}`), 2000);
            }
        }
        if (type === 0x83) console.log(`salió el miembro ${body[0]}`);
        if (type === 0x84 && body.length >= 2) onLdn(body[0], body[1], body.subarray(2));
    }
});
tcp.on('close', () => { console.log('conexión cerrada'); process.exit(0); });
tcp.on('error', (e) => { console.error(e.message); process.exit(1); });
