# Arma el paquete para la tarjeta SD: dist\lanywhere-<versión>.zip, para descomprimir en la raíz de la SD.
#
#   .\scripts\package.ps1 [-Version 0.1.0]
#
# Antes hay que compilar el sysmodule (console\, con "make") y la app (app\, con "make").
param([string]$Version = '0.1.0')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$sd = Join-Path $root 'console\out\sd'
$app = Join-Path $root 'app\lanywhere.nro'
foreach ($f in @("$sd\atmosphere\contents\4200000000000010\exefs.nsp", $app)) {
    if (-not (Test-Path $f)) { throw "Falta ${f}: compila primero el sysmodule y la app." }
}

$stage = Join-Path ([IO.Path]::GetTempPath()) "lanywhere-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
Copy-Item $sd $stage -Recurse
Copy-Item $app "$stage\switch\lanywhere.nro"
@"
LANywhere $Version

1. Descomprime este zip en la raíz de la tarjeta SD (acepta reemplazar archivos).
2. Reinicia la consola.
3. Abre "LANywhere" desde el menú de homebrew: debe decir "Conectado".
4. Abre un juego, entra en el modo inalámbrico local y crea un grupo o únete a uno.

Necesita Atmosphère 1.12.x y Wi-Fi con internet. Para jugar con desconocidos, usa en la consola un usuario
con un apodo (no tu nombre real): algunos juegos muestran el nombre del usuario dentro de la partida.
Más información: https://github.com/augustocco/lanywhere

---

1. Unzip this file to the root of the SD card (allow it to replace files).
2. Restart the console.
3. Open "LANywhere" from the homebrew menu: it should say "Connected".
4. Open a game, go to its local wireless mode and create a group or join one.

Requires Atmosphère 1.12.x and Wi-Fi with internet access. To play with strangers, use a console user with a
nickname (not your real name): some games show the user name inside the match.
More information: https://github.com/augustocco/lanywhere
"@ | Set-Content "$stage\lanywhere-README.txt" -Encoding utf8

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$zip = Join-Path $dist "lanywhere-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Remove-Item $stage -Recurse -Force
Write-Host "Paquete: $zip"
