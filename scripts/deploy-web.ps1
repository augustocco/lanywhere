# Publica la web (site/) en el servidor con HTTPS: instala Caddy, que sirve la página y reenvía /api/public a los datos
# públicos del servidor de salas (127.0.0.1:11463), y saca el certificado de Let's Encrypt solo.
#
#   .\scripts\deploy-web.ps1 -VmHost 1.2.3.4 -Key $HOME\.ssh\mi_clave -Domain lanywhere.example
#
# Antes: el dominio (y www, si lo usas) debe apuntar a la IP del servidor con un registro A, y el cortafuegos del
# proveedor debe dejar pasar TCP 80 y 443 (80 lo usa Let's Encrypt para comprobar el dominio).
param(
    [Parameter(Mandatory)][string]$VmHost,
    [Parameter(Mandatory)][string]$Key,
    [Parameter(Mandatory)][string]$Domain,
    [string]$User = 'ubuntu'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ssh = @('-i', $Key, '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new')

& scp @ssh -r (Join-Path $root 'site') "${User}@${VmHost}:/tmp/lanywhere-site"
if ($LASTEXITCODE -ne 0) { throw 'scp falló' }

$remote = @"
set -e
DOMAIN='$Domain'
"@ + "`n" + @'
# Caddy desde su repositorio oficial.
if ! command -v caddy >/dev/null 2>&1; then
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq debian-keyring debian-archive-keyring apt-transport-https curl gnupg >/dev/null
  curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | sudo gpg --batch --yes --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
  curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' | sudo tee /etc/apt/sources.list.d/caddy-stable.list >/dev/null
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq caddy >/dev/null
fi
# Caddy también entra en las actualizaciones automáticas (viene de su propio repositorio).
printf 'Unattended-Upgrade::Origins-Pattern {
	"origin=cloudsmith/caddy/stable";
};
' | sudo tee /etc/apt/apt.conf.d/51unattended-upgrades-caddy >/dev/null
sudo rm -rf /var/www/lanywhere
sudo install -d /var/www/lanywhere
sudo cp -r /tmp/lanywhere-site/. /var/www/lanywhere/
rm -rf /tmp/lanywhere-site
sudo tee /etc/caddy/Caddyfile >/dev/null <<CADDY
{
	# Solo HTTP/1.1 y HTTP/2: el cortafuegos no abre UDP 443 (HTTP/3).
	servers {
		protocols h1 h2
	}
}
$DOMAIN, www.$DOMAIN {
	encode gzip
	header {
		Strict-Transport-Security "max-age=31536000"
		X-Content-Type-Options nosniff
		Referrer-Policy no-referrer
		X-Frame-Options DENY
		Permissions-Policy "camera=(), microphone=(), geolocation=(), interest-cohort=()"
		# La página es un solo archivo con su CSS y su JS dentro, y solo pide datos a sí misma.
		Content-Security-Policy "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'; object-src 'none'"
		-Server
	}
	handle /api/public* {
		reverse_proxy 127.0.0.1:11463
	}
	handle {
		root * /var/www/lanywhere
		file_server
	}
}
CADDY
# El iptables de algunas imágenes solo deja pasar SSH: abrir 80 y 443 una vez.
for p in 80 443; do
  sudo iptables -C INPUT -p tcp --dport $p -m state --state NEW -j ACCEPT 2>/dev/null || sudo iptables -I INPUT 5 -p tcp --dport $p -m state --state NEW -j ACCEPT
done
sudo netfilter-persistent save >/dev/null 2>&1 || true
sudo systemctl enable caddy >/dev/null 2>&1
sudo systemctl reload caddy 2>/dev/null || sudo systemctl restart caddy
sleep 2
systemctl is-active caddy
'@
$remote = $remote -replace "`r`n", "`n"
$script = Join-Path ([IO.Path]::GetTempPath()) 'lanywhere-web-install.sh'
[IO.File]::WriteAllText($script, $remote)
& scp @ssh $script "${User}@${VmHost}:/tmp/lanywhere-web-install.sh"
if ($LASTEXITCODE -ne 0) { throw 'scp del instalador falló' }
& ssh @ssh "${User}@${VmHost}" 'bash /tmp/lanywhere-web-install.sh; s=$?; rm -f /tmp/lanywhere-web-install.sh; exit $s'
if ($LASTEXITCODE -ne 0) { throw 'la instalación remota falló' }
Write-Host "Web publicada en https://$Domain (el certificado tarda unos segundos la primera vez)."
