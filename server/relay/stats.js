'use strict';
// Panel de actividad del servidor de salas. Solo escucha en 127.0.0.1: se ve desde el PC del administrador por un
// túnel SSH (scripts/panel.ps1). Todo es agregado: ni IPs ni nada personal, solo apodos y códigos de sala.
//
// Guarda en el directorio de estado (stats.json) la historia: cada minuto de las últimas 24 h y cada día de los
// últimos 30, más los contadores y la actividad reciente.

const fs = require('fs');
const path = require('path');
const http = require('http');
const crypto = require('crypto');

const MINUTES = 24 * 60, DAYS = 30, EVENTS = 100;
const SEARCH_WINDOW_MS = 5 * 60 * 1000;  // "consolas buscando": las que buscaron grupos en los últimos 5 minutos

function create({ stateDir, port, snapshot, traffic, capBytes, paused }) {
    const file = path.join(stateDir, 'stats.json');
    const state = { counters: {}, minutes: [], days: [], events: [] };
    try {
        Object.assign(state, JSON.parse(fs.readFileSync(file, 'utf8')));
    } catch { /* primera vez */ }

    // Huella de IP (sal aleatoria, solo en memoria) → último momento en que buscó: desde un juego o con la app abierta.
    const recent = { game: new Map(), app: new Map() };
    const salt = crypto.randomBytes(16);
    let todaySeen = { game: new Set(), app: new Set() };
    let lastBytes = traffic.bytes;
    const started = Date.now();

    const dayKey = () => new Date().toISOString().slice(0, 10);
    function today() {
        const key = dayKey();
        let d = state.days[state.days.length - 1];
        if (!d || d.date !== key) {
            d = { date: key, peakOnline: 0, bytes: 0, searchers: 0, appUsers: 0, counters: {} };
            state.days.push(d);
            if (state.days.length > DAYS) state.days.splice(0, state.days.length - DAYS);
            todaySeen = { game: new Set(), app: new Set() };
        }
        return d;
    }

    function count(name, n = 1) {
        state.counters[name] = (state.counters[name] || 0) + n;
        const d = today();
        d.counters[name] = (d.counters[name] || 0) + n;
    }

    function event(text) {
        state.events.push({ t: Date.now(), text });
        if (state.events.length > EVENTS) state.events.splice(0, state.events.length - EVENTS);
    }

    // kind: 'game' (un juego buscando grupo) o 'app' (la lista de la app, que se refresca sola).
    function noteSearch(ip, kind = 'game') {
        const h = crypto.createHash('sha256').update(salt).update(ip).digest('hex').slice(0, 16);
        const d = today();
        recent[kind].set(h, Date.now());
        todaySeen[kind].add(h);
        d.searchers = todaySeen.game.size;
        d.appUsers = todaySeen.app.size;
    }

    function active(kind) {
        const limit = Date.now() - SEARCH_WINDOW_MS;
        for (const [h, t] of recent[kind]) if (t < limit) recent[kind].delete(h);
        return recent[kind].size;
    }

    function tick() {
        const s = snapshot();
        let bytes = traffic.bytes - lastBytes;
        if (bytes < 0) bytes = traffic.bytes;  // empezó un mes nuevo y el contador volvió a cero
        lastBytes = traffic.bytes;
        state.minutes.push({ t: Date.now(), online: s.online, rooms: s.rooms, groups: s.groups, bytes });
        if (state.minutes.length > MINUTES) state.minutes.splice(0, state.minutes.length - MINUTES);
        const d = today();
        d.peakOnline = Math.max(d.peakOnline, s.online);
        d.bytes += bytes;
        save();
    }

    function save() {
        try {
            fs.writeFileSync(file, JSON.stringify(state));
        } catch { /* el panel no debe tumbar el servidor */ }
    }

    function gameNames() {
        try {
            return JSON.parse(fs.readFileSync(path.join(stateDir, 'games.json'), 'utf8'));
        } catch {
            return {};
        }
    }

    function report() {
        today();
        return {
            now: { ...snapshot(), searching: active('game'), appOpen: active('app'), paused: paused(), uptimeMs: Date.now() - started },
            traffic: { month: traffic.month, bytes: traffic.bytes, cap: capBytes },
            counters: state.counters,
            today: state.days[state.days.length - 1],
            minutes: state.minutes,
            days: state.days,
            events: state.events.slice().reverse(),
            gameNames: gameNames(),
        };
    }

    setInterval(tick, 60000).unref();

    if (port) {
        const page = path.join(__dirname, 'dashboard.html');
        const server = http.createServer((req, res) => {
            if (req.url === '/api/stats') {
                res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
                return res.end(JSON.stringify(report()));
            }
            if (req.url === '/' || req.url === '/index.html') {
                fs.readFile(page, (err, html) => {
                    res.writeHead(err ? 500 : 200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
                    res.end(err ? 'Falta dashboard.html junto a server.js' : html);
                });
                return;
            }
            res.writeHead(404);
            res.end();
        });
        server.on('error', (e) => console.log(new Date().toISOString(), `panel: ${e.message}`));
        server.listen(port, '127.0.0.1');
    }

    return { count, event, noteSearch, save };
}

module.exports = { create };
