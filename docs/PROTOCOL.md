# Protocolo del servidor de salas (LANywhere)

Puerto **11460**: TCP para el control (salas, mensajes de búsqueda/unión) y UDP para los paquetes del juego.
Todos los enteros en **little-endian**.

## Salas y direcciones virtuales

- Una sala tiene un **código** de 6 caracteres (`A-Z` sin `I`/`O`, `2-9`), hasta **8 miembros** e índices 0..7.
- Las salas van en la red **10.13.0.0/16** (máscara 255.255.0.0), la misma de LAN Play: cada miembro recibe una IP
  única en todo el servidor (un emulador conserva la suya). Así consolas y emuladores ven las mismas direcciones,
  también las que el juego lleva dentro de sus paquetes. Broadcast: **10.13.255.255** (y 255.255.255.255 igual).
- Clientes antiguos (sin el byte de funciones en CREATE/JOIN): sus salas siguen en **10.114.0.(índice + 1)**/24,
  broadcast 10.114.0.255, sin cruzarse con LAN Play; no pueden entrar en salas 10.13 (ERROR 8).

## TCP (control)

Tramas: `u16 longitud` (del resto) · `u8 tipo` · cuerpo.

| Tipo | Sentido | Cuerpo |
|---|---|---|
| 0x01 CREATE | C→S | `u64 game_id` · `char nick[32]` · `u8 funciones` (opcional) |
| 0x02 JOIN | C→S | `char code[6]` · `char nick[32]` · `u64 game_id` (opcional) · `u8 funciones` (opcional) |
| 0x03 LEAVE | C→S | — |
| 0x04 RELAY | C→S | `u8 destino` (índice; 0xFF = todos los demás) · datos |
| 0x05 PING | C→S | — (cada 5 s; sin tráfico en 20 s el servidor cierra) |
| 0x06 RESUME | C→S | `u32 token` · `char[6] código` — vuelve al mismo puesto (índice, IP, token) tras un corte; se contesta con ROOM |
| 0x07 PUBLISH | C→S | NetworkInfo de ldn (0x480 bytes) del grupo del anfitrión; vacío lo retira |
| 0x08 FIND | C→S | `u64 id del juego` (0 = todos); no hace falta estar en una sala |
| 0x87 GROUPS | S→C | `u8 n` · n × (`char[6] código` · NetworkInfo): grupos abiertos con sitio, hasta 6 |
| 0x81 ROOM | S→C | `char code[6]` · `u8 mi_índice` · `u32 mi_ip` · `u32 token` · `u64 game_id` · `u8 n` · n × miembro |
| 0x82 MEMBER_JOINED | S→C | miembro |
| 0x83 MEMBER_LEFT | S→C | `u8 índice` |
| 0x84 RELAY | S→C | `u8 origen` · datos |
| 0x85 ERROR | S→C | `u8 código` (1 sala no existe, 2 sala llena, 3 juego distinto, 4 petición no válida, 5 el puesto de RESUME ya no existe, 6 servidor en pausa por el tope mensual de tráfico, 7 demasiadas peticiones, 8 hay que actualizar el cliente) · texto |
| 0x86 PONG | S→C | — |

Miembro: `u8 índice` · `u32 ip` · `char nick[32]`.

`game_id` es el identificador de comunicación local del juego: al unirse, el servidor comprueba que coincide con
el de la sala (no se mezclan juegos distintos). Con `game_id` 0 en el JOIN no se comprueba.

`funciones` (bit 0): el cliente entiende la red 10.13.0.0/16. Lo manda el sysmodule actual; la sala toma el del CREATE.

## UDP (datos)

Datagramas: `u32 magic 0x594E414C ("LANY")` · `u8 tipo` · cuerpo.

| Tipo | Sentido | Cuerpo |
|---|---|---|
| 0x01 REGISTER | C→S | `u32 token` (el de ROOM; repetir hasta recibir REGISTERED) |
| 0x02 DATA | C→S | `u32 token` · `u32 ip_destino` · `u16 puerto_origen` · `u16 puerto_destino` · carga |
| 0x03 KEEPALIVE | C→S | `u32 token` (cada 5 s: mantiene abierta la traducción NAT del router) |
| 0x81 REGISTERED | S→C | — |
| 0x82 DATA | S→C | `u32 ip_origen` · `u16 puerto_origen` · `u16 puerto_destino` · carga |

- El servidor identifica al remitente por el `token`, no por su dirección: si el router cambia el puerto
  público, el siguiente REGISTER/KEEPALIVE/DATA actualiza la dirección.
- DATA a una IP de miembro se reenvía solo a ese miembro; a broadcast, a todos los demás miembros registrados.
- Carga máxima: 1472 bytes (un datagrama UDP con MTU 1500, como en una LAN). Con las cabeceras, un DATA grande pasa de
  1500 bytes y viaja fragmentado: el cortafuegos del proveedor debe dejar pasar los fragmentos UDP (ver el README).

## Cortes de conexión

Si la conexión TCP se cierra sin LEAVE, el servidor guarda el puesto del miembro 60 s (los demás no reciben
MEMBER_LEFT). La consola reintenta RESUME cada 2 s durante 55 s y vuelve a registrar su UDP. Pasa, por ejemplo,
cuando el juego activa el modo inalámbrico local y la consola reconecta el Wi-Fi.

## Control de ldn por RELAY

Con una sala activa, ldn_mitm no usa broadcast UDP ni TCP en la red local: manda sus mensajes con RELAY como
`u8 LdnMsg` · datos. 1 SCAN (a todos), 2 SCAN_RESP (NetworkInfo, 0x480 bytes), 3 CONNECT (NodeInfo, 0x40),
4 SYNC (NetworkInfo, del anfitrión a cada estación), 5 DISCONNECT. Las IP de los nodos son las virtuales de la sala.

## Grupos abiertos

Si la consola no está en una sala privada, **crear grupo** en el juego crea una sala nueva (con el id del juego) y
publica el grupo con PUBLISH cada vez que cambia. **Buscar grupo** hace FIND con el id del juego (además del broadcast
en la red local) y muestra esos grupos en el menú del juego; **unirse** a uno hace JOIN con su código y luego CONNECT.
Al cerrar o dejar el grupo, la consola sale de la sala. `public=0` en `sdmc:/lanywhere/config.ini` lo desactiva.

## Pasarela LAN Play (emuladores)

En **UDP 11451** el servidor habla el protocolo de switch-lan-play (`u8 tipo` · cuerpo: 0 keepalive, 1 IPv4,
2 ping, 3 fragmento IPv4), el que usa Ryujinx en su modo "LAN Play". Dentro viaja el modo local de ldn_mitm en LAN:
búsqueda por broadcast UDP al 11452, unión por TCP al 11452 del anfitrión y los paquetes del juego por UDP. La
pasarela (`server/relay/lanplay.js`):

- Reenvía entre emuladores como un servidor de LAN Play normal, por la IP de destino.
- Responde a la búsqueda de un emulador con los grupos abiertos de consolas (salas 10.13); al unirse, el emulador
  entra en la sala como un miembro virtual con su propia IP.
- Pregunta cada 3 s a los emuladores si tienen grupo y publica los que encuentra como grupos abiertos; una consola
  que se une llega al emulador por una conexión TCP que abre la pasarela en su nombre.
- Responde al ping (ICMP) en las IPs de las consolas, para que un emulador no elija una IP ocupada.
- Reensambla los fragmentos IP y de LAN Play que van a una consola, y parte en fragmentos IP los paquetes de más de
  1400 bytes que construye para un emulador.

## Seguridad y límites del servidor

- **Apodos:** el nombre en la sala es el apodo de `config.ini` (`apodo=`; el sysmodule crea uno al azar), nunca el
  nombre de usuario de la consola. El servidor lo limpia (letras, números, espacio, `. _ -`; 2 a 20 caracteres; filtro de
  palabras ofensivas → "Jugador NNNN") y reescribe con esos apodos los nombres de los nodos de todo NetworkInfo que
  pasa por él (PUBLISH, SCAN_RESP y SYNC).
- **UDP:** solo se acepta desde la misma IP pública que la conexión TCP del miembro (evita la reflexión con un token
  adivinado o capturado).
- **Límites por IP:** 12 conexiones; por minuto, 10 CREATE, 30 JOIN/RESUME y 120 FIND. Máximo 3000 salas.
- **Tope mensual de tráfico de salida** (`LANYWHERE_CAP_GB`, 8000 por defecto): se cuenta
  todo lo enviado (con cabeceras) y se guarda en `LANYWHERE_STATE_DIR/traffic.json`; al superarlo el servidor responde
  ERROR 6 y deja de reenviar hasta el día 1 del mes siguiente (UTC).
