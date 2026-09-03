#include <string>
#include <sstream>
#include "mad/deploy.hpp"
#include "mad/core.hpp"
#include "mad/net.hpp"

namespace mad {

// экранирование двойных кавычек в sh-строке
static inline std::string dq(const std::string& s) {
    std::string r; r.reserve(s.size()+8);
    for (char c: s) { if (c=='"') r += "\\\""; else r += c; }
    return r;
}

// --- DEPLOY NGINX ------------------------------------------------------------
// Замечание: добавлен ключ --warning=no-timestamp к tar, чтобы подавить
// «временная метка ... в будущем».
std::string build_nginx_deploy_cmd(const Config& C,
                                   const std::string& remote_tar,
                                   const std::string& remote_sql,
                                   const std::string& remote_day)
{
    std::ostringstream sh;

    sh
    << "set -e; SUDO=\"\"; "
    << "if [ \"$(id -u)\" -ne 0 ]; then "
    << "  if command -v sudo >/dev/null 2>&1; then SUDO=\"sudo\"; else echo 'sudo not found' 1>&2; exit 1; fi; "
    << "fi; "
    << "echo '🔧 Проверяю окружение (nginx/mysql/tar)' 1>&2; "
    << "command -v nginx >/dev/null || { echo 'nginx not found' 1>&2; exit 1; }; "
    << "command -v tar   >/dev/null || { echo 'tar not found' 1>&2; exit 1; }; "
    << "WEBROOT=\"" << dq(C.remote_site_dir) << "\"; "
    << "REMOTE_TAR=\"" << dq(remote_tar) << "\"; "
    << "REMOTE_SQL=\"" << dq(remote_sql) << "\"; "
    << "REMOTE_DAY=\"" << dq(remote_day) << "\"; "

    // директории
    << "echo '📦 Подготавливаю директории сайта' 1>&2; "
    << "$SUDO mkdir -p \"$WEBROOT\"; "
    << "$SUDO rm -rf \"$WEBROOT.old\"; "
    << "$SUDO mv \"$WEBROOT\" \"$WEBROOT.old\" 2>/dev/null || true; "
    << "$SUDO mkdir -p \"$WEBROOT\"; "

    // распаковка (с подавлением предупреждений)
    << "echo '📤 Распаковка архива сайта' 1>&2; "
    << "if command -v pv >/dev/null 2>&1; then "
    << "  pv -f -p -t -e -r -b \"$REMOTE_TAR\" | tar --warning=no-timestamp -xzf - -C \"$WEBROOT\"; "
    << "else "
    << "  tar --warning=no-timestamp -xzf \"$REMOTE_TAR\" -C \"$WEBROOT\" --checkpoint=500 --checkpoint-action=echo=. ; echo; "
    << "fi; "

    // права
    << "echo '🧰 Выставляю права' 1>&2; "
    << "$SUDO chown -R www-data:www-data \"$WEBROOT\" || true; "
    << "find \"$WEBROOT\" -type d -exec $SUDO chmod 755 {} \\; || true; "
    << "find \"$WEBROOT\" -type f -exec $SUDO chmod 644 {} \\; || true; "

    // БД
    << "echo '🗄️  Подготавливаю БД и пользователя (MariaDB)' 1>&2; "
    << "SQL=\""
       "CREATE DATABASE IF NOT EXISTS \\\"" << dq(C.db_name) << "\\\" "
       "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci; "
       "CREATE USER IF NOT EXISTS '" << C.db_user << "'@'localhost' IDENTIFIED BY '" << C.db_pass << "'; "
       "CREATE USER IF NOT EXISTS '" << C.db_user << "'@'127.0.0.1' IDENTIFIED BY '" << C.db_pass << "'; "
       "GRANT ALL ON \\\"" << dq(C.db_name) << "\\\".* TO '" << C.db_user << "'@'localhost', '" << C.db_user << "'@'127.0.0.1';"
    << "\"; "
    << "$SUDO sh -lc \"echo \\\"$SQL\\\" | mysql\"; "

    << "echo '📦 Бэкап прежней БД (мягко)' 1>&2; "
    << "$SUDO sh -lc \"/usr/bin/mysqldump " << C.db_name
       << " > \\\"$REMOTE_DAY/db_old.sql\\\" 2>/dev/null || true\"; "

    << "if [ -s \"$REMOTE_SQL\" ]; then "
    << "  echo '⬇️  Импортирую дамп' 1>&2; "
    << "  $SUDO sh -lc \"/usr/bin/mysql " << C.db_name << " < \\\"$REMOTE_SQL\\\"\"; "
    << "fi; "

    // nginx конфиги/переключатель (не меняем здесь — ниже отдельные команды switch_*)
    << "echo '🔀 Проверка конфигурации nginx' 1>&2; "
    << "$SUDO nginx -t; "
    << "$SUDO systemctl reload nginx || $SUDO nginx -s reload; "

    // счетчик таблиц чисто информативно
    << "CNT=$($SUDO sh -lc \"mysql -N -B -e 'SELECT COUNT(*) FROM information_schema.tables "
       "WHERE table_schema=\\\"" << dq(C.db_name) << "\\\";'\" 2>/dev/null || echo 0); "
    << "echo '   📊 Таблиц в БД после импорта: '\"$CNT\" 1>&2; "

    // ротация старых бэкапов (по образцу apache)
    << "echo '🧹 Ротация старых бэкапов (7+ дней)' 1>&2; "
    << "find \"" << dq(C.remote_backup_base) << "\" -maxdepth 1 -type d "
       "-regextype posix-extended -regex '.*/[0-9]{4}-[0-9]{2}-[0-9]{2}$' -mtime +7 -exec rm -rf {} + ; "

    << "exit 0;";

    return sh.str();
}

// --- DEPLOY APACHE (пока без изменений) -------------------------------------
std::string build_apache_deploy_cmd(const Config& C,
                                    const std::string& remote_tar
                                    //const std::string& remote_sql,
                                    //const std::string& remote_day
					)
{
    // пока просто распаковка
    std::ostringstream sh;
    sh
    << "set -e; SUDO=\"\"; "
    << "if [ \"$(id -u)\" -ne 0 ]; then "
    << "  if command -v sudo >/dev/null 2>&1; then SUDO=\"sudo\"; else echo 'sudo not found' 1>&2; exit 1; fi; "
    << "fi; "
    << "WEBROOT=\"" << dq(C.remote_site_dir) << "\"; "
    << "REMOTE_TAR=\"" << dq(remote_tar) << "\"; "
    << "echo '📤 Распаковка архива сайта (apache)' 1>&2; "
    << "$SUDO mkdir -p \"$WEBROOT\"; "
    << "if command -v pv >/dev/null 2>&1; then "
    << "  pv -f -p -t -e -r -b \"$REMOTE_TAR\" | tar --warning=no-timestamp -xzf - -C \"$WEBROOT\"; "
    << "else "
    << "  tar --warning=no-timestamp -xzf \"$REMOTE_TAR\" -C \"$WEBROOT\" --checkpoint=500 --checkpoint-action=echo=. ; echo; "
    << "fi; "
    << "$SUDO chown -R www-data:www-data \"$WEBROOT\" || true; "
    << "find \"$WEBROOT\" -type d -exec $SUDO chmod 755 {} \\; || true; "
    << "find \"$WEBROOT\" -type f -exec $SUDO chmod 644 {} \\; || true; "
    << "exit 0;";
    return sh.str();
}


// --- SWITCH: nginx -> remote -------------------------------------------------
// Возвращаем бекенд на REMOTE (C.proxy_target:80/443 — в твоём сетапе 198).
std::string build_nginx_switch_to_remote_cmd(const Config& C)
{
    std::ostringstream sh;
    sh
    << "set -e; SUDO=\"\"; "
    << "if [ \"$(id -u)\" -ne 0 ]; then "
    << "  if command -v sudo >/dev/null 2>&1; then SUDO=\"sudo\"; else echo 'sudo not found' 1>&2; exit 1; fi; "
    << "fi; "
    << "CONF=\"/etc/nginx/conf.d/" << dq(C.server_name) << ".backend.conf\"; "
    << "echo '🔁 Переключаю nginx BACKEND → remote (" << dq(C.proxy_target) << ")' 1>&2; "
    << "cat > /tmp/_backend.conf <<EOF\n"
       "upstream " << C.server_name << "_backend {\n"
       "    server " << C.proxy_target << ":80;\n"
       "}\n"
       "map $host $backend_scheme { default http; }\n"
       "EOF\n"
    << "$SUDO mv /tmp/_backend.conf \"$CONF\"; "
    << "$SUDO nginx -t && $SUDO systemctl reload nginx || $SUDO nginx -s reload; "
    << "echo '✅ BACKEND: remote' 1>&2; "
    << "exit 0;";
    return sh.str();
}

} // namespace mad
