# Sysmodule de LANywhere

Fork de [ldn_mitm](https://github.com/spacemeowx2/ldn_mitm) (spacemeowx2 y colaboradores, GPL-2.0). Sustituye el
servicio `ldn` del sistema para que el modo inalámbrico local vaya por la red; LANywhere añade:

- `lanywhere_relay.*`: cliente del servidor de salas (salas, grupos abiertos, reconexión, apodo, registro).
- `lanywhere_service.*`: el servicio `lanywhr` que usa la app (crear o unirse a una sala, estado, grupos, apodo).
- `bsd_spy.*`: mitm de `bsd:u` para las aplicaciones: los paquetes hacia la red virtual de la sala (10.114.0.0/24) se
  reenvían por el servidor.
- Cambios en `lan_discovery.*` e `ldn_icommunication.cpp`: con una sala activa, buscar, crear y unirse a grupos van por
  el servidor en lugar de por broadcast y TCP en la red local.

Compilar: `make` (necesita `Atmosphere-libs` 1.12.1 y `libtesla`, submódulos). Salida en `out/sd`.

Ver el [README del proyecto](../README.md) y [docs/PROTOCOL.md](../docs/PROTOCOL.md).
