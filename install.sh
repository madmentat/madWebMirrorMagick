#!/usr/bin/env bash
set -euo pipefail

UI_PORT="${MADUI_PORT:-8790}"
UI_BIND="${MADUI_BIND:-0.0.0.0}"
PREFIX="${MADWEB_PREFIX:-/usr/local}"
BIN_DST="$PREFIX/bin/madwebmirror"
HELPER_DST="$PREFIX/libexec/madweb-helper"
AGENT_DST="$PREFIX/libexec/madweb-agent"
SERVICE_USER="madbackup"

if [[ "${EUID}" -eq 0 ]]; then
  SUDO=""
else
  SUDO="sudo"
  echo "🔐 Проверяю административный сеанс sudo…"
  sudo -v
fi

need_cmd() {
  command -v "$1" >/dev/null 2>&1
}

have_libssh_headers() {
  [[ -f /usr/include/libssh/libssh.h || -f /usr/local/include/libssh/libssh.h ]]
}

install_deps_debian() {
  echo "📦 Устанавливаю зависимости сборки и SSH transport…"
  $SUDO apt-get update
  $SUDO apt-get install -y build-essential libssh-dev openssh-client sudo
}

if ! need_cmd g++ || ! need_cmd make || ! need_cmd ssh || ! need_cmd ssh-keygen || ! need_cmd ssh-copy-id || ! have_libssh_headers; then
  if need_cmd apt-get; then
    install_deps_debian
  else
    echo "❌ Нужны g++, make, OpenSSH client, sudo и libssh development headers. Автоустановка пока реализована для apt-based систем." >&2
    exit 1
  fi
fi

if ! need_cmd visudo; then
  echo "❌ Не найден visudo. Нужен пакет sudo для безопасной установки ограниченного helper-а." >&2
  exit 1
fi

echo "🛠️  Собираю madWebMirrorMagick…"
make clean
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

echo "👤 Готовлю системного пользователя $SERVICE_USER…"
if ! getent passwd "$SERVICE_USER" >/dev/null 2>&1; then
  $SUDO useradd --system --home-dir /var/lib/madwebmirror --create-home --shell /usr/sbin/nologin "$SERVICE_USER"
fi

SERVICE_GROUP="$(getent passwd "$SERVICE_USER" | cut -d: -f4)"
SERVICE_GROUP_NAME="$(getent group "$SERVICE_GROUP" | cut -d: -f1)"

echo "📁 Готовлю системные каталоги…"
$SUDO install -d -m 0755 "$PREFIX/bin" "$PREFIX/libexec"
$SUDO install -d -m 0750 -o root -g "$SERVICE_GROUP_NAME" /etc/madwebmirror /srv/madwebmirror /srv/madwebmirror/sites
$SUDO install -d -m 0750 -o "$SERVICE_USER" -g "$SERVICE_GROUP_NAME" /var/lib/madwebmirror /var/lib/madwebmirror/sites
$SUDO install -d -m 0700 -o "$SERVICE_USER" -g "$SERVICE_GROUP_NAME" /var/lib/madwebmirror/ssh /var/lib/madwebmirror/.ssh

# A sudo-launched madUI should use one canonical system config instead of
# accidentally creating /root/.config/madbackuper.conf.
if [[ ! -e /etc/madbackuper.conf ]]; then
  CONFIG_TMP="$(mktemp)"
  cat >"$CONFIG_TMP" <<'EOF'
# madWebMirrorMagick system configuration
# Complete setup in madUI. File is root:madbackup 0640.
EOF
  $SUDO install -o root -g "$SERVICE_GROUP_NAME" -m 0640 "$CONFIG_TMP" /etc/madbackuper.conf
  rm -f "$CONFIG_TMP"
fi

if [[ ! -e /etc/madwebmirror/tunnels.conf ]]; then
  TUNNELS_TMP="$(mktemp)"
  cat >"$TUNNELS_TMP" <<'EOF'
# madWebMirrorMagick SSH tunnel manager
# id|route(primary/fallback)|direction(local/remote)|bind_host|bind_port|target_host|target_port|enabled
# Same id on primary and fallback creates one automatic failover group.
EOF
  $SUDO install -o "$SERVICE_USER" -g "$SERVICE_GROUP_NAME" -m 0640 "$TUNNELS_TMP" /etc/madwebmirror/tunnels.conf
  rm -f "$TUNNELS_TMP"
fi

echo "📥 Устанавливаю основной бинарник и privileged helper…"
$SUDO install -m 0755 ./madbackuper "$BIN_DST"
$SUDO ln -sfn "$BIN_DST" "$PREFIX/bin/madbackuper"
$SUDO install -o root -g root -m 0755 ./madweb-helper "$HELPER_DST"
$SUDO install -o root -g root -m 0755 ./madweb-agent "$AGENT_DST"

SUDOERS_TMP="$(mktemp)"
trap 'rm -f "$SUDOERS_TMP"' EXIT
cat >"$SUDOERS_TMP" <<EOF
# madWebMirrorMagick: the service account may invoke only the root-owned helper.
# The helper exposes a fixed verb set and validates all arguments itself.
$SERVICE_USER ALL=(root) NOPASSWD: $HELPER_DST *
EOF
$SUDO visudo -cf "$SUDOERS_TMP" >/dev/null
$SUDO install -o root -g root -m 0440 "$SUDOERS_TMP" /etc/sudoers.d/madwebmirror
rm -f "$SUDOERS_TMP"
trap - EXIT

TUNNEL_UNIT_TMP="$(mktemp)"
cat >"$TUNNEL_UNIT_TMP" <<EOF
[Unit]
Description=madWebMirrorMagick dual-proxy SSH tunnel supervisor
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_GROUP_NAME
ExecStart=$BIN_DST tunnels --config=/etc/madbackuper.conf --tunnels=/etc/madwebmirror/tunnels.conf
Restart=always
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
LockPersonality=true
RestrictSUIDSGID=true

[Install]
WantedBy=multi-user.target
EOF
$SUDO install -o root -g root -m 0644 "$TUNNEL_UNIT_TMP" /etc/systemd/system/madwebmirror-tunnels.service
rm -f "$TUNNEL_UNIT_TMP"
$SUDO systemctl daemon-reload

echo
echo "✅ Runtime privilege model установлен:"
echo "   • $SERVICE_USER не имеет shell-login;"
echo "   • sudo/root пароль программе не нужен;"
echo "   • privileged операции доступны только через $HELPER_DST;"
echo "   • автономный failover agent: $AGENT_DST;"
echo "   • web-копии создаются под /srv/madwebmirror/sites/<site-id>;"
echo "   • SSH keys хранятся в /var/lib/madwebmirror/ssh;"
echo "   • SSH trust store: /var/lib/madwebmirror/.ssh/known_hosts;"
echo "   • tunnel policy: /etc/madwebmirror/tunnels.conf;"
echo "   • madwebmirror-tunnels.service установлен, но включается после настройки tunnel policy."
echo
echo "✨ Сейчас будет запущен madUI."
echo "   Откройте напечатанный ниже адрес в браузере."
echo "   После открытия страницы подтвердите одноразовый код в этом терминале."
echo "   Пароль sudo самой программе не передаётся и не сохраняется."
echo

exec $SUDO "$BIN_DST" ui --ui-bind="$UI_BIND" --ui-port="$UI_PORT"
