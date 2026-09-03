#!/usr/bin/env bash
set -euo pipefail

UI_PORT="${MADUI_PORT:-8790}"
UI_BIND="${MADUI_BIND:-0.0.0.0}"
PREFIX="${MADWEB_PREFIX:-/usr/local}"
BIN_DST="$PREFIX/bin/madwebmirror"

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

install_deps_debian() {
  echo "📦 Устанавливаю зависимости сборки…"
  $SUDO apt-get update
  $SUDO apt-get install -y build-essential libssh-dev
}

if ! need_cmd g++ || ! need_cmd make || ! pkg-config --exists libssh 2>/dev/null; then
  if need_cmd apt-get; then
    install_deps_debian
  else
    echo "❌ Нужны g++, make и libssh development headers. Автоустановка пока реализована для apt-based систем." >&2
    exit 1
  fi
fi

echo "🛠️  Собираю madWebMirrorMagick…"
make clean
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

echo "📥 Устанавливаю $BIN_DST"
$SUDO install -d -m 0755 "$PREFIX/bin"
$SUDO install -m 0755 ./madbackuper "$BIN_DST"
$SUDO ln -sfn "$BIN_DST" "$PREFIX/bin/madbackuper"

echo
echo "✨ Установка завершена. Сейчас будет запущен madUI."
echo "   Откройте напечатанный ниже адрес в браузере."
echo "   После открытия страницы подтвердите одноразовый код в этом терминале."
echo "   Пароль sudo самой программе не передаётся и не сохраняется."
echo

exec $SUDO "$BIN_DST" ui --ui-bind="$UI_BIND" --ui-port="$UI_PORT"
