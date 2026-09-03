#include "mad/deploy.hpp"

#include <cctype>
#include <sstream>

namespace mad {
namespace {

bool safe_site_id(const std::string& value) {
    if (value.empty()) return false;
    for (const unsigned char c : value) {
        if (!std::isalnum(c) && c != '.' && c != '-' && c != '_') return false;
    }
    return true;
}

std::string build_common_deploy_cmd(const Config& cfg,
                                    const std::string& remote_tar,
                                    const std::string& remote_sql,
                                    const std::string& remote_db_cnf,
                                    const std::string& remote_day,
                                    const char* server_kind) {
    const std::string webroot = cfg.remote_site_dir;
    const std::string staging = webroot + ".new";
    const std::string old = webroot + ".old";

    std::ostringstream sh;
    sh << "set -Eeuo pipefail; umask 077; "
       << "WEBROOT=" << shell_quote(webroot) << "; "
       << "STAGING=" << shell_quote(staging) << "; "
       << "OLD=" << shell_quote(old) << "; "
       << "REMOTE_TAR=" << shell_quote(remote_tar) << "; "
       << "REMOTE_SQL=" << shell_quote(remote_sql) << "; "
       << "DB_CNF=" << shell_quote(remote_db_cnf) << "; "
       << "REMOTE_DAY=" << shell_quote(remote_day) << "; "
       << "DB_NAME=" << shell_quote(cfg.db_name) << "; "
       << "trap 'rm -f -- \"$DB_CNF\"; rm -rf -- \"$STAGING\"' EXIT; "
       << "command -v tar >/dev/null || { echo 'tar not found' >&2; exit 20; }; "
       << "command -v mysql >/dev/null || { echo 'mysql client not found' >&2; exit 21; }; "
       << "command -v mysqldump >/dev/null || { echo 'mysqldump not found' >&2; exit 22; }; "
       << "test -s \"$REMOTE_TAR\" || { echo 'site archive is empty' >&2; exit 23; }; "
       << "test -s \"$DB_CNF\" || { echo 'database credentials file is missing' >&2; exit 24; }; "
       << "echo '📦 Готовлю staging-каталог (" << server_kind << ")' >&2; "
       << "rm -rf -- \"$STAGING\"; mkdir -p -- \"$STAGING\"; "
       << "tar --warning=no-timestamp -xzf \"$REMOTE_TAR\" -C \"$STAGING\"; "
       << "find \"$STAGING\" -type d -exec chmod 755 {} +; "
       << "find \"$STAGING\" -type f -exec chmod 644 {} +; "
       << "echo '📦 Сохраняю текущую БД' >&2; "
       << "mysqldump --defaults-extra-file=\"$DB_CNF\" --single-transaction --quick --routines --triggers --events --no-tablespaces "
       << "\"$DB_NAME\" > \"$REMOTE_DAY/db_old.sql\"; chmod 600 \"$REMOTE_DAY/db_old.sql\"; "
       << "echo '🔁 Переключаю webroot' >&2; "
       << "rm -rf -- \"$OLD\"; "
       << "if [ -e \"$WEBROOT\" ]; then mv -- \"$WEBROOT\" \"$OLD\"; fi; "
       << "mv -- \"$STAGING\" \"$WEBROOT\"; "
       << "if [ -s \"$REMOTE_SQL\" ]; then "
       << "  echo '🗄️ Импортирую БД' >&2; "
       << "  if ! mysql --defaults-extra-file=\"$DB_CNF\" \"$DB_NAME\" < \"$REMOTE_SQL\"; then "
       << "    echo '❌ Импорт БД не удался — возвращаю прежний webroot' >&2; "
       << "    rm -rf -- \"$WEBROOT\"; if [ -e \"$OLD\" ]; then mv -- \"$OLD\" \"$WEBROOT\"; fi; exit 30; "
       << "  fi; "
       << "fi; "
       << "trap - EXIT; rm -f -- \"$DB_CNF\"; "
       << "echo '✅ Развёртывание завершено' >&2;";
    return sh.str();
}

} // namespace

std::string build_nginx_deploy_cmd(const Config& cfg,
                                   const std::string& remote_tar,
                                   const std::string& remote_sql,
                                   const std::string& remote_db_cnf,
                                   const std::string& remote_day) {
    return build_common_deploy_cmd(cfg, remote_tar, remote_sql, remote_db_cnf, remote_day, "nginx");
}

std::string build_apache_deploy_cmd(const Config& cfg,
                                    const std::string& remote_tar,
                                    const std::string& remote_sql,
                                    const std::string& remote_db_cnf,
                                    const std::string& remote_day) {
    return build_common_deploy_cmd(cfg, remote_tar, remote_sql, remote_db_cnf, remote_day, "apache2");
}

std::string resolved_switch_script(const Config& cfg) {
    if (!cfg.switch_script.empty()) return cfg.switch_script;
    std::string id = cfg.server_name;
    const auto dot = id.find('.');
    if (dot != std::string::npos) id.resize(dot);
    if (!safe_site_id(id)) return {};
    return "/root/setup_" + id + "_nginx.sh";
}

static std::string build_switch_cmd(const Config& cfg, const char* mode) {
    const std::string script = resolved_switch_script(cfg);
    if (script.empty()) return "echo 'invalid switch_script/server_name' >&2; exit 2";

    // Никаких универсальных sudo-команд: разрешается только заранее установленный
    // root-owned switch script и только два фиксированных аргумента local/remote.
    if (cfg.remote_user == "root") {
        return shell_quote(script) + " " + mode;
    }
    return "sudo -n -- " + shell_quote(script) + " " + mode;
}

std::string build_nginx_switch_to_local_cmd(const Config& cfg) {
    return build_switch_cmd(cfg, "local");
}

std::string build_nginx_switch_to_remote_cmd(const Config& cfg) {
    return build_switch_cmd(cfg, "remote");
}

} // namespace mad
