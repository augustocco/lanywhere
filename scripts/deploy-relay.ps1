# Instala o actualiza el servidor de salas (server/relay/server.js) en la VM como servicio systemd
# "lanywhere" (TCP/UDP 11460), con su propio usuario sin privilegios.
#
#   .\scripts\deploy-relay.ps1 -VmHost 1.2.3.4 -Key $HOME\.ssh\mi_clave
param(
    [Parameter(Mandatory)][string]$VmHost,
    [Parameter(Mandatory)][string]$Key,
    [string]$User = 'ubuntu'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ssh = @('-i', $Key, '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new')

# El servidor y su panel de actividad (stats.js + dashboard.html, solo en 127.0.0.1; ver scripts/panel.ps1).
$files = 'server.js', 'stats.js', 'dashboard.html'
& scp @ssh ($files | ForEach-Object { Join-Path $root "server\relay\$_" }) "${User}@${VmHost}:/tmp/"
if ($LASTEXITCODE -ne 0) { throw 'scp falló' }

$remote = @'
set -e
# Lo que necesita el servidor en una Ubuntu recién creada: Node.js e iptables persistente.
if ! command -v node >/dev/null 2>&1 || ! command -v netfilter-persistent >/dev/null 2>&1; then
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq nodejs iptables-persistent >/dev/null
fi
sudo id lanywhere >/dev/null 2>&1 || sudo useradd --system --home /opt/lanywhere --shell /usr/sbin/nologin lanywhere
sudo install -d -o lanywhere -g lanywhere /opt/lanywhere
for f in server.js stats.js dashboard.html; do
  sudo install -o lanywhere -g lanywhere -m 0644 "/tmp/$f" "/opt/lanywhere/$f"
  rm -f "/tmp/$f"
done
sudo tee /etc/systemd/system/lanywhere.service >/dev/null <<'UNIT'
[Unit]
Description=LANywhere: servidor de salas (TCP/UDP 11460)
After=network-online.target
Wants=network-online.target

[Service]
User=lanywhere
WorkingDirectory=/opt/lanywhere
ExecStart=/usr/bin/node server.js --port 11460
Restart=always
RestartSec=3
# Contador de tráfico del mes (tope configurable; ver server.js).
StateDirectory=lanywhere
Environment=LANYWHERE_STATE_DIR=/var/lib/lanywhere
Environment=LANYWHERE_CAP_GB=8000
# Sin privilegios y sin acceso a lo que no necesita.
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
PrivateDevices=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectKernelLogs=true
ProtectControlGroups=true
ProtectClock=true
ProtectHostname=true
RestrictNamespaces=true
RestrictRealtime=true
RestrictSUIDSGID=true
LockPersonality=true
CapabilityBoundingSet=
AmbientCapabilities=
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
SystemCallArchitectures=native
MemoryMax=512M
TasksMax=64

[Install]
WantedBy=multi-user.target
UNIT
# Si la imagen trae reglas de iptables que solo dejan pasar SSH, abrir 11460 una vez (si no, no estorba).
sudo iptables -C INPUT -p udp --dport 11460 -j ACCEPT 2>/dev/null || sudo iptables -I INPUT 5 -p udp --dport 11460 -j ACCEPT
sudo iptables -C INPUT -p tcp --dport 11460 -m state --state NEW -j ACCEPT 2>/dev/null || sudo iptables -I INPUT 5 -p tcp --dport 11460 -m state --state NEW -j ACCEPT
# El lan-play del principio ya no se usa: fuera el servicio y su puerto.
if systemctl list-unit-files lan-play.service >/dev/null 2>&1; then sudo systemctl disable --now lan-play >/dev/null 2>&1 || true; fi
while sudo iptables -C INPUT -p udp --dport 11451 -j ACCEPT 2>/dev/null; do sudo iptables -D INPUT -p udp --dport 11451 -j ACCEPT; done
while sudo iptables -C INPUT -p tcp --dport 11451 -m state --state NEW -j ACCEPT 2>/dev/null; do sudo iptables -D INPUT -p tcp --dport 11451 -m state --state NEW -j ACCEPT; done
sudo netfilter-persistent save >/dev/null
# Parches de seguridad de Ubuntu automáticos.
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq unattended-upgrades >/dev/null
printf 'APT::Periodic::Update-Package-Lists "1";
APT::Periodic::Unattended-Upgrade "1";
' | sudo tee /etc/apt/apt.conf.d/20auto-upgrades >/dev/null
sudo systemctl daemon-reload
sudo systemctl enable lanywhere >/dev/null 2>&1
sudo systemctl restart lanywhere
sleep 1
systemctl is-active lanywhere
'@ -replace "`r`n", "`n"
# Se copia como archivo y se ejecuta: pasarlo a ssh por la entrada estándar desde PowerShell no llega bien.
$script = Join-Path ([IO.Path]::GetTempPath()) 'lanywhere-install.sh'
[IO.File]::WriteAllText($script, $remote)
& scp @ssh $script "${User}@${VmHost}:/tmp/lanywhere-install.sh"
if ($LASTEXITCODE -ne 0) { throw 'scp del instalador falló' }
& ssh @ssh "${User}@${VmHost}" 'bash /tmp/lanywhere-install.sh; s=$?; rm -f /tmp/lanywhere-install.sh; exit $s'
if ($LASTEXITCODE -ne 0) { throw 'la instalación remota falló' }
Write-Host "Servidor de salas desplegado en ${VmHost}:11460"
