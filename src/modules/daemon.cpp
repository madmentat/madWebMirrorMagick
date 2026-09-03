#include "mad/daemon.hpp"

#include <libssh/sftp.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "mad/deploy.hpp"
#include "mad/net.hpp"

namespace mad {
namespace {

std::string readlink_self() {
    std::vector<char> buf(1024);
    while (true) {
        const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        if (n < 0) return {};
        if (static_cast<std::size_t>(n) < buf.size() - 1) {
            buf[static_cast<std::size_t>(n)] = '\0';
            return std::string(buf.data());
        }
        buf.resize(buf.size() * 2);
    }
}

int ssh_exec_one(const Config& cfg, const std::string& command, bool print_out = true) {
    std::string error;
    ssh_session session = ssh_connect_authenticated(cfg, error);
    if (!session) {
        std::cerr << "❌ " << error << '\n';
        return -1;
    }
    const int rc = ssh_exec(session, command, print_out);
    ssh_disconnect_and_free(session);
    return rc;
}

std::string sudo_shell_command(const Config& cfg, const std::string& inner) {
    if (cfg.remote_user == "root") return "sh -lc " + shell_quote(inner);
    if (cfg.remote_sudo_pass.empty()) {
        return "sudo -n sh -lc " + shell_quote(inner);
    }
    return "printf %s " + shell_quote(cfg.remote_sudo_pass) +
           " | sudo -S -p '' sh -lc " + shell_quote(inner);
}

int install_local_backup_timer(const Config& cfg, const std::string& exe_path) {
    if (::geteuid() != 0) {
        std::cerr << "❌ Установка systemd unit требует однократного запуска от root.\n";
        return 1;
    }

    if (run_local("install -m 0755 " + shell_quote(exe_path) + " /usr/local/bin/madbackuper") != 0) return 1;

    int hh = 0;
    int mm = 0;
    if (!parse_hhmm(cfg.schedule_hhmm, hh, mm)) return 1;

    {
        std::ofstream service("/etc/systemd/system/madbackuper-backup.service", std::ios::trunc);
        if (!service) return 1;
        service << R"([Unit]
Description=madWebMirrorMagick backup and deploy
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/madbackuper backup
User=root
Nice=10
IOSchedulingClass=best-effort
IOSchedulingPriority=6
)";
    }

    {
        std::ofstream timer("/etc/systemd/system/madbackuper-backup.timer", std::ios::trunc);
        if (!timer) return 1;
        timer << "[Unit]\nDescription=Daily madWebMirrorMagick backup\n\n"
              << "[Timer]\nOnCalendar=*-*-* "
              << (hh < 10 ? "0" : "") << hh << ':' << (mm < 10 ? "0" : "") << mm
              << ":00\nPersistent=true\nRandomizedDelaySec=30\n\n"
              << "[Install]\nWantedBy=timers.target\n";
    }

    if (run_local("systemctl daemon-reload") != 0) return 1;
    if (run_local("systemctl enable --now madbackuper-backup.timer") != 0) return 1;
    return 0;
}

int uninstall_local_backup_timer() {
    if (::geteuid() != 0) return 1;
    run_local("systemctl disable --now madbackuper-backup.timer >/dev/null 2>&1", false);
    run_local("rm -f /etc/systemd/system/madbackuper-backup.timer /etc/systemd/system/madbackuper-backup.service", false);
    return run_local("systemctl daemon-reload", false);
}

int install_remote_watchdog(const Config& cfg) {
    if (cfg.target_server != "nginx" || !cfg.switch_to_local) return 0;

    const std::string switch_script = resolved_switch_script(cfg);
    if (switch_script.empty()) {
        std::cerr << "❌ Не удалось определить switch_script\n";
        return 1;
    }

    std::ostringstream script;
    script << "#!/usr/bin/env bash\n"
           << "set -Eeuo pipefail\n"
           << "PROXY_TARGET=" << shell_quote(cfg.proxy_target) << "\n"
           << "HTTP_PORT=" << cfg.local_http_port << "\n"
           << "INTERVAL=" << cfg.health_interval_sec << "\n"
           << "FAIL_LIMIT=" << cfg.health_failures << "\n"
           << "RECOVER_LIMIT=" << cfg.health_recoveries << "\n"
           << "COOLDOWN=" << cfg.switch_cooldown_sec << "\n"
           << "SWITCH=" << shell_quote(switch_script) << "\n"
           << "state=unknown; fails=0; oks=0; last_switch=0\n"
           << "while true; do\n"
           << "  now=$(date +%s)\n"
           << "  if curl -fsS -m 5 \"http://${PROXY_TARGET}:${HTTP_PORT}/\" >/dev/null 2>&1; then\n"
           << "    fails=0; oks=$((oks+1))\n"
           << "    if [[ \"$state\" == backup && $oks -ge $RECOVER_LIMIT && $((now-last_switch)) -ge $COOLDOWN ]]; then\n"
           << "      \"$SWITCH\" remote && state=primary && last_switch=$now\n"
           << "    elif [[ \"$state\" == unknown && $oks -ge $RECOVER_LIMIT ]]; then state=primary; fi\n"
           << "  else\n"
           << "    oks=0; fails=$((fails+1))\n"
           << "    if [[ \"$state\" != backup && $fails -ge $FAIL_LIMIT && $((now-last_switch)) -ge $COOLDOWN ]]; then\n"
           << "      \"$SWITCH\" local && state=backup && last_switch=$now\n"
           << "    fi\n"
           << "  fi\n"
           << "  sleep \"$INTERVAL\"\n"
           << "done\n";

    const std::string local_script = "/tmp/madbackuper-watchdog.sh";
    const std::string local_unit = "/tmp/madbackuper-watchdog.service";
    {
        std::ofstream out(local_script, std::ios::trunc);
        out << script.str();
    }
    {
        std::ofstream out(local_unit, std::ios::trunc);
        out << R"([Unit]
Description=madWebMirrorMagick remote failover watchdog
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/madbackuper-watchdog
Restart=always
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
)";
    }

    std::string error;
    ssh_session session = ssh_connect_authenticated(cfg, error);
    if (!session) {
        std::cerr << "❌ " << error << '\n';
        return 1;
    }

    sftp_session sftp = sftp_new(session);
    if (!sftp || sftp_init(sftp) != SSH_OK) {
        if (sftp) sftp_free(sftp);
        ssh_disconnect_and_free(session);
        return 1;
    }

    int sftp_error = 0;
    const bool script_ok = sftp_upload_file_progress(session, sftp, local_script,
        "/tmp/madbackuper-watchdog.sh", "watchdog", &sftp_error, 0700) == 0;
    const bool unit_ok = sftp_upload_file_progress(session, sftp, local_unit,
        "/tmp/madbackuper-watchdog.service", "watchdog unit", &sftp_error, 0600) == 0;
    sftp_free(sftp);
    ::unlink(local_script.c_str());
    ::unlink(local_unit.c_str());

    if (!script_ok || !unit_ok) {
        ssh_disconnect_and_free(session);
        return 1;
    }

    const std::string install =
        "set -e; install -m 0755 /tmp/madbackuper-watchdog.sh /usr/local/bin/madbackuper-watchdog; "
        "install -m 0644 /tmp/madbackuper-watchdog.service /etc/systemd/system/madbackuper-watchdog.service; "
        "rm -f /tmp/madbackuper-watchdog.sh /tmp/madbackuper-watchdog.service; "
        "systemctl daemon-reload; systemctl enable --now madbackuper-watchdog.service";

    const int rc = ssh_exec(session, sudo_shell_command(cfg, install), true);
    ssh_disconnect_and_free(session);
    return rc == 0 ? 0 : 1;
}

int uninstall_remote_watchdog(const Config& cfg) {
    if (cfg.target_server != "nginx") return 0;
    const std::string command =
        "systemctl disable --now madbackuper-watchdog.service >/dev/null 2>&1 || true; "
        "rm -f /etc/systemd/system/madbackuper-watchdog.service /usr/local/bin/madbackuper-watchdog; "
        "systemctl daemon-reload";
    return ssh_exec_one(cfg, sudo_shell_command(cfg, command), true) == 0 ? 0 : 1;
}

} // namespace

bool check_local_site_ok(const Config& cfg) {
    const std::string url = cfg.health_url.empty()
        ? "http://127.0.0.1:" + std::to_string(cfg.local_http_port) + "/"
        : cfg.health_url;

    std::ostringstream cmd;
    cmd << "curl -fsSL -m 5 ";
    if (!cfg.health_host_header.empty()) cmd << "-H " << shell_quote("Host: " + cfg.health_host_header) << ' ';
    cmd << shell_quote(url) << " >/dev/null 2>&1";
    return std::system(cmd.str().c_str()) == 0;
}

int remote_switch_to_local_nginx(const Config& cfg) {
    return ssh_exec_one(cfg, build_nginx_switch_to_local_cmd(cfg), true);
}

int remote_switch_to_remote_nginx(const Config& cfg) {
    return ssh_exec_one(cfg, build_nginx_switch_to_remote_cmd(cfg), true);
}

int run_monitor_loop(const Config& cfg) {
    if (cfg.target_server != "nginx") {
        std::cerr << "❌ Failover monitor сейчас поддерживает только nginx\n";
        return 1;
    }

    int fails = 0;
    int oks = 0;
    bool backup_mode = false;
    auto last_switch = std::chrono::steady_clock::now() - std::chrono::seconds(cfg.switch_cooldown_sec);

    std::cout << "🌀 Monitor: fail=" << cfg.health_failures
              << ", recover=" << cfg.health_recoveries << '\n';
    while (true) {
        const bool ok = check_local_site_ok(cfg);
        if (ok) {
            fails = 0;
            ++oks;
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - last_switch).count();
            if (backup_mode && oks >= cfg.health_recoveries && elapsed >= cfg.switch_cooldown_sec) {
                if (remote_switch_to_remote_nginx(cfg) == 0) {
                    backup_mode = false;
                    last_switch = std::chrono::steady_clock::now();
                }
            }
        } else {
            oks = 0;
            ++fails;
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - last_switch).count();
            if (!backup_mode && fails >= cfg.health_failures && elapsed >= cfg.switch_cooldown_sec) {
                if (remote_switch_to_local_nginx(cfg) == 0) {
                    backup_mode = true;
                    last_switch = std::chrono::steady_clock::now();
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(cfg.health_interval_sec));
    }
}

int daemon_install(const Config& cfg, const std::string& self_path) {
    const std::string exe = self_path.empty() ? readlink_self() : self_path;
    if (exe.empty()) return 1;

    const int local_rc = install_local_backup_timer(cfg, exe);
    const int remote_rc = install_remote_watchdog(cfg);
    if (local_rc == 0 && remote_rc == 0) {
        std::cout << "✅ Установлены backup timer и удалённый watchdog\n";
        return 0;
    }
    if (local_rc != 0) std::cerr << "❌ Не установлен локальный backup timer\n";
    if (remote_rc != 0) std::cerr << "❌ Не установлен удалённый watchdog\n";
    return 1;
}

int daemon_uninstall(const Config& cfg) {
    const int remote_rc = uninstall_remote_watchdog(cfg);
    const int local_rc = uninstall_local_backup_timer();
    return local_rc == 0 && remote_rc == 0 ? 0 : 1;
}

} // namespace mad
