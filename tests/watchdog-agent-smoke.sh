#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat >"$TMP/curl" <<'EOF'
#!/usr/bin/env bash
[[ "${FAKE_PRIMARY_HEALTH:-down}" == "up" ]]
EOF

cat >"$TMP/switch" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$1" >>"${SWITCH_LOG:?}"
EOF

chmod 0755 "$TMP/curl" "$TMP/switch"

cat >"$TMP/watchdog.conf" <<EOF
health_url=http://primary.invalid/
health_host_header=example.test
switch_script=$TMP/switch
state_file=$TMP/state
lock_file=$TMP/watchdog.lock
curl_path=$TMP/curl
interval_sec=1
failures=1
recoveries=1
cooldown_sec=0
EOF

export SWITCH_LOG="$TMP/switch.log"

# Первый запуск обязан явно применить primary, а не просто поверить state=unknown.
FAKE_PRIMARY_HEALTH=up "$ROOT/madweb-agent" watchdog --once --config="$TMP/watchdog.conf"
[[ "$(tail -n1 "$SWITCH_LOG")" == "remote" ]]
grep -qx 'mode=primary' "$TMP/state"

# После рестарта агент повторно сверяет реальный маршрут даже при сохранённом primary.
: >"$SWITCH_LOG"
FAKE_PRIMARY_HEALTH=up "$ROOT/madweb-agent" watchdog --once --config="$TMP/watchdog.conf"
[[ "$(cat "$SWITCH_LOG")" == "remote" ]]

# При отказе primary автономный агент переводит frontend на локальную копию.
: >"$SWITCH_LOG"
FAKE_PRIMARY_HEALTH=down "$ROOT/madweb-agent" watchdog --once --config="$TMP/watchdog.conf"
[[ "$(cat "$SWITCH_LOG")" == "local" ]]
grep -qx 'mode=backup' "$TMP/state"
grep -qx 'primary_healthy=false' "$TMP/state"

"$ROOT/madweb-agent" status --config="$TMP/watchdog.conf" | grep -qx 'mode=backup'

# Небезопасный относительный switch_script должен быть отклонён до запуска.
sed "s|switch_script=$TMP/switch|switch_script=relative-script|" "$TMP/watchdog.conf" >"$TMP/bad.conf"
if "$ROOT/madweb-agent" watchdog --once --config="$TMP/bad.conf"; then
    echo "relative switch_script was unexpectedly accepted" >&2
    exit 1
fi

echo "watchdog agent smoke test: OK"
