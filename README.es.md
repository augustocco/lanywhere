# LANywhere

**Español** · [English](README.md)

> 🧪 **Beta: buscamos probadores.** Todo funciona con una sola consola, pero falta probar partidas completas entre
> consolas reales en casas distintas. Si lo pruebas, cuéntanos cómo fue (aunque haya fallado) con un
> [informe de prueba](../../issues/new?template=test-report.yml). Los juegos probados hasta ahora están en el
> [issue de la beta](../../issues?q=label%3Abeta).

### Tu multijugador local, ahora en línea.

El multijugador local, pero sin que tus amigos tengan que estar en la misma habitación. LANywhere lleva el **modo
inalámbrico local** de tus juegos a internet: sin PC, sin configurar nada y sin escribir IPs.

Creas un grupo en el juego como siempre ("inalámbrico local" → "crear grupo") y jugadores de todo el mundo pueden
unirse; buscas grupo y ves los que hay abiertos en internet. Para jugar solo con un amigo, una sala privada con un
código de 6 letras.

> Proyecto independiente y sin ánimo de lucro, sin relación con ningún fabricante de consolas ni de juegos. Solo
> lleva por internet el tráfico de red del modo inalámbrico local: no modifica juegos, no usa claves ni cuentas, y no
> da acceso a ningún servicio en línea oficial.

## Qué necesitas

- Una consola con **Atmosphère 1.12.x**.
- **Nada más que configurar:** LANywhere usa el servidor público del proyecto, que ya está funcionando. No necesitas
  un servidor propio, ni PC, ni escribir ninguna IP.
- **Wi-Fi con internet.**
- El **mismo juego y la misma versión** que los demás (igual que en el modo local de verdad).

## Instalación

1. Descarga `lanywhere-<versión>.zip` de [Releases](../../releases).
2. Descomprímelo en la **raíz de la tarjeta SD** (acepta reemplazar archivos; si tenías ldn_mitm, se sustituye).
3. Reinicia la consola.
4. Abre **LANywhere** desde el menú de homebrew: arriba a la derecha debe decir **Conectado**.

## Cómo se juega

1. Abre el juego.
2. Entra en el modo **inalámbrico local** (en algunos juegos, "juego local").
3. **Crea un grupo** (aparecerá para todo el mundo) o **busca uno** para unirte.

La app **LANywhere** muestra los grupos abiertos ahora, tu apodo y la sala privada:

Todo se puede usar también con la pantalla táctil.

| Botón | Qué hace |
|---|---|
| Tocar un grupo (o ↑ ↓) | Elegir un grupo abierto; **A** (o tocarlo otra vez) abre su juego, y ese grupo es el que te espera en "buscar grupo" |
| A | Crear una sala privada: te da un código para pasárselo a tu amigo |
| Y | Unirse a una sala privada con su código |
| B | Salir de la sala privada |
| X | Mostrar u ocultar tus grupos al resto del mundo |
| − | Cambiar tu apodo |

Mientras estás en una sala privada, tus grupos solo los ve esa sala.

## Seguridad y privacidad

- **Apodo, nunca tu nombre de usuario.** Los demás te ven con un apodo (uno al azar hasta que elijas el tuyo). El
  servidor filtra los apodos ofensivos.
- ⚠️ **Dentro de la partida**, algunos juegos muestran el nombre del usuario de la consola, porque lo envía el propio
  juego. Para jugar con desconocidos, **crea en la consola un usuario con un apodo** y juega con él.
- **Nadie ve tu IP.** Todo pasa por el servidor; los demás jugadores solo ven una IP virtual (10.114.0.x).
- **Sin chat**: los juegos en modo local no tienen, y LANywhere no añade ninguno.
- **Mantén los juegos actualizados.** Con grupos abiertos recibes datos de consolas desconocidas, como en cualquier
  juego en línea; algunos juegos antiguos tenían fallos de red graves que ya se corrigieron en sus actualizaciones.
- Si prefieres no salir de tu círculo: pulsa **X** en la app para ocultar tus grupos y usa salas privadas.

## Cómo funciona

```
 juego ──ldn──▶ sysmodule (fork de ldn_mitm) ──TCP/UDP 11460──▶ servidor de salas ◀── otras consolas
```

- **Sysmodule** (`console/`): un fork de [ldn_mitm](https://github.com/spacemeowx2/ldn_mitm). Sustituye el servicio
  `ldn` (inalámbrico local) del sistema; los grupos se publican, buscan y unen a través del servidor, y los paquetes
  del juego hacia la red virtual de la sala se reenvían por él.
- **Servidor** (`server/relay/server.js`): Node.js sin dependencias. Salas de hasta 8 consolas, grupos abiertos,
  reconexión automática si se corta el Wi-Fi, límites contra abusos y un tope de tráfico mensual.
- **App** (`app/`): el panel LANywhere.

El protocolo está en [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Tu propio servidor (opcional)

**No hace falta para jugar.** LANywhere viene listo para usar el servidor público del proyecto, que ya está
funcionando. Esta sección es solo para quien quiera montar el suyo (por ejemplo, uno más cerca de su región). Para usar
otro servidor, crea `sdmc:/lanywhere/config.ini`:

```ini
server=1.2.3.4
port=11460
```

Sirve cualquier máquina virtual pequeña con Linux en la nube o en casa; por ejemplo, una instancia `t4g.small` de AWS
o equivalente. Requisitos mínimos:

- 1 vCPU y 1 GB de RAM (el servidor usa muy poco: unas decenas de MB).
- Ubuntu 22.04 o 24.04 (u otra distribución con Node.js 18 o más nuevo).
- IP pública y los puertos **TCP y UDP 11460** abiertos en el cortafuegos del proveedor.
- Lo que más cuenta es el **tráfico de salida**: todo el juego pasa por el servidor. Ajusta `LANYWHERE_CAP_GB` a lo que
  incluya tu plan para no llevarte sorpresas.

El servidor es un solo archivo: `node server/relay/server.js --port 11460`. Variables: `LANYWHERE_CAP_GB` (tope de tráfico
de salida al mes, 8000 por defecto; al llegar, se pausa hasta el mes siguiente) y `LANYWHERE_STATE_DIR` (dónde guarda el
contador).

Para instalarlo como servicio en una Ubuntu (con acceso por SSH con clave):

```powershell
.\scripts\deploy-relay.ps1 -VmHost <IP de la VM> -Key $HOME\.ssh\mi_clave
```

Instala Node.js si falta y deja el servidor como servicio systemd aislado (sin privilegios), con parches de seguridad
automáticos. Recomendado: permitir SSH solo desde tu IP.

## Compilar

Con [devkitPro](https://devkitpro.org) (devkitA64 y libnx):

```sh
git submodule update --init --recursive   # Atmosphere-libs (1.12.1) y libtesla
make -C console                           # sysmodule → console/out/sd
make -C app                               # app → app/lanywhere.nro
```

```powershell
.\scripts\package.ps1 -Version 0.1.0     # dist\lanywhere-0.1.0.zip
```

Pruebas del servidor: `node server/relay/test.js`. Para probar con una sola consola, `server/relay/fake-peer.js`
simula a otro jugador (`--abiertas`: busca y se une a un grupo abierto; `--anfitrion`: publica un grupo).

Para depurar en la consola: `sdmc:/lanywhere/relay.log` (siempre, hasta 256 KB) y, si existe el archivo
`sdmc:/lanywhere/debug.flag`, también `bsd.log` con los sockets del juego.

## Créditos y licencia

- [ldn_mitm](https://github.com/spacemeowx2/ldn_mitm) de spacemeowx2 y colaboradores: la base del sysmodule.
- [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) y libnx.
- [libtesla](https://github.com/WerWolv/libtesla) (overlay de ldnmitm_config).

GPL-2.0, como ldn_mitm: ver [LICENSE](LICENSE).
