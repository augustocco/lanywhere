# Abre el panel de actividad del servidor de salas en el navegador.
#
#   .\scripts\panel.ps1 -VmHost 1.2.3.4 -Key $HOME\.ssh\mi_clave
#
# El panel solo escucha dentro del servidor (127.0.0.1:11461): este script abre un túnel SSH hasta él con tu clave y
# lanza el navegador en http://localhost:11461. Mientras esta ventana siga abierta, el panel se ve; al cerrarla (o
# con Ctrl+C), el túnel se cierra. No hace falta abrir ningún puerto nuevo en el servidor.
param(
    [Parameter(Mandatory)][string]$VmHost,
    [Parameter(Mandatory)][string]$Key,
    [string]$User = 'ubuntu',
    [int]$LocalPort = 11461
)
$ErrorActionPreference = 'Stop'

$ssh = Start-Process ssh -ArgumentList @('-i', $Key, '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes',
                                         '-N', '-L', "${LocalPort}:127.0.0.1:11461", "${User}@${VmHost}") -NoNewWindow -PassThru
# Esperar a que el túnel responda antes de abrir el navegador.
$ready = $false
for ($i = 0; $i -lt 30 -and -not $ssh.HasExited; $i++) {
    try {
        Invoke-WebRequest "http://localhost:$LocalPort/api/stats" -UseBasicParsing -TimeoutSec 2 | Out-Null
        $ready = $true
        break
    } catch {
        Start-Sleep -Milliseconds 500
    }
}
if (-not $ready) {
    if (-not $ssh.HasExited) { $ssh.Kill() }
    throw 'No se pudo abrir el túnel hasta el panel (¿clave, IP o puerto local ocupado?).'
}
Start-Process "http://localhost:$LocalPort"
Write-Host "Panel abierto en http://localhost:$LocalPort. Cierra esta ventana (o pulsa Ctrl+C) para cerrar el túnel."
try {
    $ssh.WaitForExit()
} finally {
    if (-not $ssh.HasExited) { $ssh.Kill() }
}
