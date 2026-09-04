#include "mad/core.hpp"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <stdexcept>

namespace mad {
namespace {

void apply_config_permissions(const std::string& path) {
    const bool system_config = path.rfind("/etc/", 0) == 0;
    const mode_t mode = system_config ? 0640 : 0600;
    if (::chmod(path.c_str(), mode) != 0) {
        throw std::runtime_error("Не удалось установить права на конфиг: " + path);
    }

    // System services run as madbackup and need read-only access to the same
    // configuration. Secrets remain unavailable to other users.
    if (system_config && ::geteuid() == 0) {
        if (passwd* pw = ::getpwnam("madbackup")) {
            if (::chown(path.c_str(), 0, pw->pw_gid) != 0) {
                throw std::runtime_error("Не удалось установить root:madbackup на конфиг: " + path);
            }
        }
    }
}

} // namespace

void save_config(const std::string& path, const Config& cfg) {
    const fs::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (ec) throw std::runtime_error("Не удалось создать каталог конфигурации: " + ec.message());
    }

    const std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) throw std::runtime_error("Не удалось создать временный конфиг: " + tmp);

    out
        << "# madWebMirrorMagick configuration\n"
        << "# Секреты sudo/root намеренно не требуются для работы madUI.\n\n"
        << "target_server=" << cfg.target_server << '\n'
        << "remote_host=" << cfg.remote_host << '\n'
        << "ssh_port=" << cfg.ssh_port << '\n'
        << "remote_user=" << cfg.remote_user << '\n'
        << "remote_pass=" << cfg.remote_pass << '\n'
        << "remote_sudo_pass=" << cfg.remote_sudo_pass << '\n'
        << "ssh_transport=" << cfg.ssh_transport << '\n'
        << "ssh_identity_file=" << cfg.ssh_identity_file << '\n'
        << "ssh_jump_primary=" << cfg.ssh_jump_primary << '\n'
        << "ssh_jump_primary_identity_file=" << cfg.ssh_jump_primary_identity_file << '\n'
        << "ssh_jump_fallback=" << cfg.ssh_jump_fallback << '\n'
        << "ssh_jump_fallback_identity_file=" << cfg.ssh_jump_fallback_identity_file << '\n'
        << "\nlocal_site_dir=" << cfg.local_site_dir << '\n'
        << "remote_site_dir=" << cfg.remote_site_dir << '\n'
        << "remote_backup_base=" << cfg.remote_backup_base << '\n'
        << "\nserver_name=" << cfg.server_name << '\n'
        << "switch_script=" << cfg.switch_script << '\n'
        << "\nphp_version=" << cfg.php_version << '\n'
        << "php_fpm_sock=" << cfg.php_fpm_sock << '\n'
        << "\ndb_user=" << cfg.db_user << '\n'
        << "db_pass=" << cfg.db_pass << '\n'
        << "db_name=" << cfg.db_name << '\n'
        << "\nproxy_target=" << cfg.proxy_target << '\n'
        << "local_http_port=" << cfg.local_http_port << '\n'
        << "local_https_port=" << cfg.local_https_port << '\n'
        << "switch_to_local=" << (cfg.switch_to_local ? "true" : "false") << '\n'
        << "\nhealth_url=" << cfg.health_url << '\n'
        << "watchdog_health_url=" << cfg.watchdog_health_url << '\n'
        << "health_host_header=" << cfg.health_host_header << '\n'
        << "health_interval_sec=" << cfg.health_interval_sec << '\n'
        << "health_failures=" << cfg.health_failures << '\n'
        << "health_recoveries=" << cfg.health_recoveries << '\n'
        << "switch_cooldown_sec=" << cfg.switch_cooldown_sec << '\n'
        << "\nssl_cert=" << cfg.ssl_cert << '\n'
        << "ssl_key=" << cfg.ssl_key << '\n'
        << "\nschedule_hhmm=" << cfg.schedule_hhmm << '\n';

    out.flush();
    if (!out) throw std::runtime_error("Ошибка записи конфигурации: " + tmp);
    out.close();

    try {
        apply_config_permissions(tmp);
    } catch (...) {
        fs::remove(tmp);
        throw;
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp);
        throw std::runtime_error("Не удалось заменить конфигурацию: " + ec.message());
    }
    apply_config_permissions(path);
}

} // namespace mad
