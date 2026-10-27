'use strict';
// Datos públicos de la red para la web (site/): totales y grupos abiertos ahora. Solo escucha en 127.0.0.1; lo publica
// el proxy HTTPS del servidor (scripts/deploy-web.ps1) en /api/public. Nunca incluye códigos de sala ni IPs: solo lo
// que ya ve cualquiera en la lista de grupos de la app (juego, apodo del anfitrión, jugadores).

const fs = require('fs');
const path = require('path');
const http = require('http');

const CACHE_MS = 5000;

function create({ stateDir, port, snapshot, groups }) {
    let names = {}, namesAt = 0, cached = null, cachedAt = 0;

    function gameNames() {
        if (Date.now() - namesAt > 60000) {
            namesAt = Date.now();
            try {
                names = JSON.parse(fs.readFileSync(path.join(stateDir, 'games.json'), 'utf8'));
            } catch {
                names = {};
            }
        }
        return names;
    }

    function report() {
        if (cached && Date.now() - cachedAt < CACHE_MS) return cached;
        const s = snapshot(), n = gameNames();
        const list = groups().map((g) => ({ game: g.game, name: n[g.game] || null, host: g.host, players: g.players, max: g.max }));
        const games = s.byGame.map((g) => ({ game: g.game, name: n[g.game] || null, groups: g.groups, players: g.players }));
        cached = JSON.stringify({ updated: new Date().toISOString(), online: s.online, groups: s.groups, players: s.players,
                                  games, list });
        cachedAt = Date.now();
        return cached;
    }

    if (!port) return;
    const server = http.createServer((req, res) => {
        if (req.method === 'GET' && (req.url === '/api/public' || req.url === '/api/public/')) {
            res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'public, max-age=5',
                                 'Access-Control-Allow-Origin': '*' });
            return res.end(report());
        }
        res.writeHead(404);
        res.end();
    });
    server.on('error', (e) => console.log(new Date().toISOString(), `web pública: ${e.message}`));
    server.listen(port, '127.0.0.1');
}

module.exports = { create };
