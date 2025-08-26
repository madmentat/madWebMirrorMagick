#include <chrono>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "mad/daemon.hpp"
#include "mad/core.hpp"
#include "mad/net.hpp"

using namespace std::chrono_literals;

namespace mad {

// Эскейп для шела
static std::string esc(const std::string& s) { return sh_escape_single(s); }

// Проверка локального фронта через curl
bool check_local_site_ok(const Config& cfg) {
    std::ostringstream cmd;
    cmd << "curl -fsSL -m 5 http://127.0.0.1:" << cfg.local_http_port << " >/dev/null 2>&1";
    int rc = std::system(cmd.str().c_str());
    return rc == 0;
}

// Быстрое подключение к SSH и выполнение команды
static int ssh_exec_one(const Config& cfg, const std::string& command, bool print_out = true) {
    ssh_session session = ssh_new();
    if (!session) { std::cerr << "❌ ssh_new()\n"; return -1; }

    ssh_options_set(session, SSH_OPTIONS_HOST, cfg.remote_host.c_str());
    ssh_options_set(session, SSH_OPTIONS_USER, cfg.remote_user.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &cfg.ssh_port);

    if (ssh_connect(session) != SSH_OK) {
        std::cerr << "❌ ssh_connect: " << ssh_get_error(session) << "\n";
        ssh_free(session);
        return -1;
    }
    if (ssh_userauth_password(session, nullptr, cfg.remote_pass.c_str()) != SSH_AUTH_SUCCESS) {
        std::cerr << "❌ ssh auth failed: " << ssh_get_error(session) << "\n";
        ssh_disconnect(session);
        ssh_free(session);
        return -1;
    }

    int rc = ssh_exec(session, command, print_out);

    ssh_disconnect(session);
    ssh_free(session);
    return rc;
}

// sudo-префикс (если надо пароль под sudo)
static std::string sudo_prefix_for(const Config& cfg) {
    return sudo_prefix(nullptr, cfg);
}

// Переключение nginx на local
int remote_switch_to_local_nginx(const Config& cfg) {
    std::ostringstream cmd;
    cmd << "sh -lc '"
        << "F=/root/setup_" << esc(cfg.server_name) << "_nginx.sh; "
        << "if [ -x \"$F\" ]; then \"$F\" local; else echo \"(no setup script)\" >&2; fi"
        << "'";
    return ssh_exec_one(cfg, cmd.str(), true);
}

// Переключение nginx на remote
int remote_switch_to_remote_nginx(const Config& cfg) {
    std::ostringstream cmd;
    cmd << "sh -lc '"
        << "F=/root/setup_" << esc(cfg.server_name) << "_nginx.sh; "
        << "if [ -x \"$F\" ]; then \"$F\" remote; else echo \"(no setup script)\" >&2; fi"
        << "'";
    return ssh_exec_one(cfg, cmd.str(), true);
}

// Заглушка для установки systemd-юнитов
int ensure_watchdog_units(const Config& cfg) {
    (void)cfg;
    // TODO: реализовать установку юнитов на 198 и 202
    return 0;
}

// Основной цикл демона
int run_daemon_loop(const Config& cfg) {
    std::cout << "🌀 Демон запущен: health-check и переключение фронта (nginx)\n";

    bool in_local_mode = false;

    // Интервал health-check берём из конфига (по умолчанию 60с)
    int health_interval_sec = cfg.health_interval_sec > 0 ? cfg.health_interval_sec : 60;

    // На старте сразу проверим сайт
    {
        bool ok = check_local_site_ok(cfg);
        if (!ok && cfg.target_server == "nginx") {
            std::cout << "⚠️  Локальный сайт на 198 не отвечает — переключаю фронт на local (202)\n";
            if (remote_switch_to_local_nginx(cfg) == 0) {
                in_local_mode = true;
            }
        }
    }

    while (true) {
        bool ok = check_local_site_ok(cfg);

        if (!ok && !in_local_mode && cfg.target_server == "nginx") {
            std::cout << "⚠️  Локальный сайт упал — переключаю фронт на local (202)\n";
            if (remote_switch_to_local_nginx(cfg) == 0) in_local_mode = true;
        } else if (ok && in_local_mode && cfg.target_server == "nginx") {
            std::cout << "✅ Локальный сайт ожил — возвращаю фронт на remote (202)\n";
            if (remote_switch_to_remote_nginx(cfg) == 0) in_local_mode = false;
        }

        std::this_thread::sleep_for(std::chrono::seconds(health_interval_sec));
    }
}

} // namespace mad
