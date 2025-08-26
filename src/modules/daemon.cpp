// src/modules/daemon.cpp
#include <chrono>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>

#include <unistd.h>     // readlink
#include <limits.h>

#include "mad/daemon.hpp"
#include "mad/core.hpp"
#include "mad/net.hpp"

using namespace std::chrono_literals;

namespace mad {

// Короткий алиас на shell-эскейп одинарными кавычками
static inline std::string esc_s(const std::string& s) { return sh_escape_single(s); }

// ─────────────────────────── Health-check локального фронта ──────────────────
bool check_local_site_ok(const Config& cfg) {
    const std::string url = cfg.health_url.empty()
        ? ("http://127.0.0.1:" + std::to_string(cfg.local_http_port) + "/")
        : cfg.health_url;

    std::ostringstream cmd;
    cmd << "curl -fsSL -m 5 ";
    if (!cfg.health_host_header.empty())
        cmd << "-H 'Host: " << cfg.health_host_header << "' ";
    cmd << esc_s(url) << " >/dev/null 2>&1";
    return std::system(cmd.str().c_str()) == 0;
}

// ─────────────────────────── Одноразовый удалённый exec ──────────────────────
static int ssh_exec_one(const Config& cfg, const std::string& command, bool print_out = true) {
    ssh_session s = ssh_new();
    if (!s) { std::cerr << "❌ ssh_new()\n"; return -1; }

    ssh_options_set(s, SSH_OPTIONS_HOST, cfg.remote_host.c_str());
    ssh_options_set(s, SSH_OPTIONS_USER, cfg.remote_user.c_str());
    ssh_options_set(s, SSH_OPTIONS_PORT, &cfg.ssh_port);

    if (ssh_connect(s) != SSH_OK) {
        std::cerr << "❌ ssh_connect: " << ssh_get_error(s) << "\n";
        ssh_free(s);
        return -1;
    }
    if (ssh_userauth_password(s, nullptr, cfg.remote_pass.c_str()) != SSH_AUTH_SUCCESS) {
        std::cerr << "❌ ssh auth failed: " << ssh_get_error(s) << "\n";
        ssh_disconnect(s);
        ssh_free(s);
        return -1;
    }

    int rc = ssh_exec(s, command, print_out);
    ssh_disconnect(s);
    ssh_free(s);
    return rc;
}

// ─────────────────────── Переключение nginx на 202 (remote) ──────────────────
int remote_switch_to_local_nginx(const Config& cfg) {
    std::ostringstream cmd;
    cmd << "sh -lc '"
        << "F=/root/setup_" << esc_s(cfg.server_name) << "_nginx.sh; "
        << "if [ -x \"$F\" ]; then \"$F\" local; else echo \"(no setup script)\" >&2; fi"
        << "'";
    return ssh_exec_one(cfg, cmd.str(), /*print_out=*/true);
}

int remote_switch_to_remote_nginx(const Config& cfg) {
    std::ostringstream cmd;
    cmd << "sh -lc '"
        << "F=/root/setup_" << esc_s(cfg.server_name) << "_nginx.sh; "
        << "if [ -x \"$F\" ]; then \"$F\" remote; else echo \"(no setup script)\" >&2; fi"
        << "'";
    return ssh_exec_one(cfg, cmd.str(), /*print_out=*/true);
}

int ensure_watchdog_units(const Config& /*cfg*/) {
    // TODO: позже можно добавить периодическую верификацию юнитов
    return 0;
}

// ───────────────────────────── Демон-цикл на 198 ─────────────────────────────
int run_daemon_loop(const Config& cfg) {
    std::cout << "🌀 Демон запущен: health-check и переключение фронта (nginx)\n";

    bool in_local_mode = false;
    const int health_interval_sec = cfg.health_interval_sec > 0 ? cfg.health_interval_sec : 60;

    // Первый прогон
    {
        const bool ok = check_local_site_ok(cfg);
        if (!ok && cfg.target_server == "nginx") {
            std::cout << "⚠️  Локальный сайт не отвечает — переключаю фронт на local (202)\n";
            if (remote_switch_to_local_nginx(cfg) == 0) in_local_mode = true;
        }
    }

    while (true) {
        const bool ok = check_local_site_ok(cfg);

        if (!ok && !in_local_mode && cfg.target_server == "nginx") {
            std::cout << "⚠️  Локальный сайт упал — переключаю фронт на local (202)\n";
            if (remote_switch_to_local_nginx(cfg) == 0) in_local_mode = true;
        } else if (ok && in_local_mode && cfg.target_server == "nginx") {
            std::cout << "✅ Локальный сайт ожил — возвращаю фронт на remote (202)\n";
            if (remote_switch_to_remote_nginx(cfg) == 0) in_local_mode = false;
        }

        std::this_thread::sleep_for(std::chrono::seconds(health_interval_sec));
    }
    // недостижимо
    // return 0;
}

// ────────────────────────── Вспомогалки для инсталляции ──────────────────────
static std::string readlink_self() {
    std::vector<char> buf(1024);
    while (true) {
        ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        if (n < 0) return {};
        if (static_cast<size_t>(n) < buf.size() - 1) {
            buf[n] = '\0';
            return std::string(buf.data());
        }
        buf.resize(buf.size() * 2);
    }
}

static inline int run_local_cmd(const std::string& cmd) {
    // используем реализацию из core.hpp (echo=true по умолчанию)
    return ::mad::run_local(cmd, true);
}

// ───────────────────────── Локальный systemd-юнит (198) ──────────────────────
static int install_local_service(const std::string& exe_path) {
    run_local_cmd("install -m 0755 " + esc_s(exe_path) + " /usr/local/bin/madbackuper");

    const char* unit_path = "/etc/systemd/system/madbackuper.service";
    {
        std::ofstream u(unit_path);
        u <<
R"([Unit]
Description=madbackuper daemon (health-check + backup scheduler)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/madbackuper --daemon
Restart=always
RestartSec=5
User=root
StandardOutput=journal
StandardError=journal
Environment=LANG=C.UTF-8

[Install]
WantedBy=multi-user.target
)";
    }

    run_local_cmd("systemctl daemon-reload");
    run_local_cmd("systemctl enable madbackuper.service");
    return run_local_cmd("systemctl restart madbackuper.service");
}

static int uninstall_local_service() {
    run_local_cmd("systemctl stop madbackuper.service >/dev/null 2>&1");
    run_local_cmd("systemctl disable madbackuper.service >/dev/null 2>&1");
    run_local_cmd("rm -f /etc/systemd/system/madbackuper.service");
    run_local_cmd("systemctl daemon-reload");
    return 0;
}

// ───────────── Удалённый watchdog (скрипт + unit) на 202 (remote_host) ───────
static int install_remote_watchdog(const Config& cfg)
{
    // --- 1) Bash-скрипт watchdog (как делали вручную)
    std::ostringstream script;
    script
    << "#!/usr/bin/env bash\n"
    << "set -Eeuo pipefail\n"
    << "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n"
    << "SERVER_NAME=" << esc_s(cfg.server_name) << "\n"
    << "PROXY_TARGET=" << esc_s(cfg.proxy_target) << "\n"
    << "HTTP_PORT=" << (cfg.local_http_port > 0 ? cfg.local_http_port : 80) << "\n"
    << "INTERVAL=" << (cfg.health_interval_sec > 0 ? cfg.health_interval_sec : 60) << "\n"
    << "SETUP=\"/root/setup_${SERVER_NAME}_nginx.sh\"\n"
    << "log(){ echo \"[watchdog] $*\"; }\n"
    << "last_state=\"unknown\"\n"
    << "while true; do\n"
    << "  if curl -fsS -m 5 \"http://${PROXY_TARGET}:${HTTP_PORT}/\" >/dev/null 2>&1; then\n"
    << "    state=\"up\"\n"
    << "  else\n"
    << "    state=\"down\"\n"
    << "  fi\n"
    << "  if [[ \"$state\" != \"$last_state\" ]]; then\n"
    << "    if [[ \"$state\" == \"up\" ]]; then\n"
    << "      log \"198 ↑ — переключаю 202 в remote\"\n"
    << "      [[ -x \"$SETUP\" ]] && \"$SETUP\" remote || log \"нет $SETUP\"\n"
    << "    else\n"
    << "      log \"198 ↓ — переключаю 202 в local\"\n"
    << "      [[ -x \"$SETUP\" ]] && \"$SETUP\" local  || log \"нет $SETUP\"\n"
    << "    fi\n"
    << "    last_state=\"$state\"\n"
    << "  fi\n"
    << "  sleep \"$INTERVAL\"\n"
    << "done\n";

    // --- 2) SSH + SFTP: кладём оба файла в /tmp на 202
    ssh_session s = ssh_new();
    if (!s) return -1;
    ssh_options_set(s, SSH_OPTIONS_HOST, cfg.remote_host.c_str());
    ssh_options_set(s, SSH_OPTIONS_USER, cfg.remote_user.c_str());
    ssh_options_set(s, SSH_OPTIONS_PORT, &cfg.ssh_port);
    if (ssh_connect(s) != SSH_OK) { ssh_free(s); return -1; }
    if (ssh_userauth_password(s, nullptr, cfg.remote_pass.c_str()) != SSH_AUTH_SUCCESS) {
        ssh_disconnect(s); ssh_free(s); return -1;
    }

    sftp_session sf = sftp_new(s);
    if (!sf || sftp_init(sf) != SSH_OK) { if (sf) sftp_free(sf); ssh_disconnect(s); ssh_free(s); return -1; }

    const std::string local_script = "/tmp/mad_watchdog_198.sh";
    { std::ofstream f(local_script); f << script.str(); }

    int sftp_err = 0;
    if (sftp_upload_file_progress(s, sf, local_script, "/tmp/mad_watchdog_198.sh", "watchdog", &sftp_err, 0644) != 0) {
        std::cerr << "❌ Не удалось загрузить watchdog на " << cfg.remote_host << " (SFTP)\n";
        sftp_free(sf); ssh_disconnect(s); ssh_free(s); return -1;
    }
    sftp_free(sf);

    // --- 3) Сборка unit-файла локально → /tmp на 202
    std::ostringstream unit;
    unit <<
R"([Unit]
Description=madbackuper remote watchdog (switch local/remote on 202)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/env bash /usr/local/bin/mad_watchdog_198.sh
Restart=always
RestartSec=5
User=root
StandardOutput=journal
StandardError=journal
Environment=LANG=C.UTF-8

[Install]
WantedBy=multi-user.target
)";
    const std::string local_unit = "/tmp/madbackuper-watchdog.service";
    { std::ofstream u(local_unit); u << unit.str(); }

    sftp_session sf2 = sftp_new(s);
    if (!sf2 || sftp_init(sf2) != SSH_OK) { if (sf2) sftp_free(sf2); ssh_disconnect(s); ssh_free(s); return -1; }
    if (sftp_upload_file_progress(s, sf2, local_unit, "/tmp/madbackuper-watchdog.service", "watchdog-unit", &sftp_err, 0644) != 0) {
        std::cerr << "❌ Не удалось загрузить unit-файл на " << cfg.remote_host << "\n";
        sftp_free(sf2); ssh_disconnect(s); ssh_free(s); return -1;
    }
    sftp_free(sf2);

    // --- 4) Повышенные права: КАЖДУЮ команду подаём как `printf %s PW | sudo -S ...`
    const std::string pw = cfg.remote_sudo_pass.empty() ? cfg.remote_pass : cfg.remote_sudo_pass;
    const std::string PW = esc_s(pw);

    // скрипт → /usr/local/bin, права 0755, удалить временный
    {
        std::ostringstream cmd;
        cmd << "sh -lc 'set -e; "
            << "printf %s " << PW << " | sudo -S -p \"\" mkdir -p /usr/local/bin; "
            << "printf %s " << PW << " | sudo -S -p \"\" install -m 0755 /tmp/mad_watchdog_198.sh /usr/local/bin/mad_watchdog_198.sh; "
            << "printf %s " << PW << " | sudo -S -p \"\" rm -f /tmp/mad_watchdog_198.sh"
            << "'";
        if (ssh_exec(s, cmd.str(), /*print_out=*/true) != 0) {
            std::cerr << "❌ Не удалось установить /usr/local/bin/mad_watchdog_198.sh на " << cfg.remote_host << "\n";
            ssh_disconnect(s); ssh_free(s); return -1;
        }
    }

    // unit → /etc/systemd/system + enable/restart
    {
        std::ostringstream cmd;
        cmd << "sh -lc 'set -e; "
            << "printf %s " << PW << " | sudo -S -p \"\" mv /tmp/madbackuper-watchdog.service /etc/systemd/system/madbackuper-watchdog.service; "
            << "printf %s " << PW << " | sudo -S -p \"\" systemctl daemon-reload; "
            << "printf %s " << PW << " | sudo -S -p \"\" systemctl enable madbackuper-watchdog.service; "
            << "printf %s " << PW << " | sudo -S -p \"\" systemctl restart madbackuper-watchdog.service"
            << "'";
        if (ssh_exec(s, cmd.str(), /*print_out=*/true) != 0) {
            std::cerr << "❌ Не удалось зарегистрировать/запустить watchdog unit на " << cfg.remote_host << "\n";
            ssh_disconnect(s); ssh_free(s); return -1;
        }
    }

    ssh_disconnect(s);
    ssh_free(s);
    return 0;
}

static int uninstall_remote_watchdog(const Config& cfg)
{
    const std::string pw = cfg.remote_sudo_pass.empty() ? cfg.remote_pass : cfg.remote_sudo_pass;
    const std::string PW = esc_s(pw);

    std::ostringstream sh;
    sh << "sh -lc 'set -e; "
       << "printf %s " << PW << " | sudo -S -p \"\" systemctl stop madbackuper-watchdog.service >/dev/null 2>&1 || true; "
       << "printf %s " << PW << " | sudo -S -p \"\" systemctl disable madbackuper-watchdog.service >/dev/null 2>&1 || true; "
       << "printf %s " << PW << " | sudo -S -p \"\" rm -f /etc/systemd/system/madbackuper-watchdog.service; "
       << "printf %s " << PW << " | sudo -S -p \"\" rm -f /usr/local/bin/mad_watchdog_198.sh; "
       << "printf %s " << PW << " | sudo -S -p \"\" systemctl daemon-reload"
       << "'";

    return ssh_exec_one(cfg, sh.str(), /*print_out=*/true);
}

// ───────────────────────── Публичные API установки демона ────────────────────
int daemon_install(const Config& cfg, const std::string& self_path)
{
    std::string exe = self_path.empty() ? readlink_self() : self_path;
    if (exe.empty()) { std::cerr << "❌ Не удалось определить путь к бинарю\n"; return 1; }

    int rc_local  = install_local_service(exe);
    int rc_remote = install_remote_watchdog(cfg);

    if (rc_local != 0)  std::cerr << "⚠️  Проблема при установке локального юнита\n";
    if (rc_remote != 0) std::cerr << "⚠️  Проблема при установке удалённого watchdog'а на " << cfg.remote_host << "\n";

    std::cout << "✅ Установка демона завершена. Логи: journalctl -u madbackuper -f\n";
    return 0; // завершаем текущий процесс — дальше рулит systemd
}

int daemon_uninstall(const Config& cfg)
{
    int rc_remote = uninstall_remote_watchdog(cfg);
    int rc_local  = uninstall_local_service();

    if (rc_remote != 0) std::cerr << "⚠️  Не удалось корректно убрать watchdog на " << cfg.remote_host << "\n";
    std::cout << "✅ Демон отключён локально. Для проверки: systemctl status madbackuper\n";
    return rc_local == 0 ? 0 : rc_local;
}

} // namespace mad
