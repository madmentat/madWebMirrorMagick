// src/modules/daemon.cpp
#include <chrono>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <map>
#include <utility>
#include <ctime>

#include <unistd.h>     // readlink
#include <limits.h>

#include "mad/daemon.hpp"
#include "mad/core.hpp"
#include "mad/net.hpp"

using namespace std::chrono_literals;

namespace mad {

// Короткий алиас на shell-эскейп одинарными кавычками
static inline std::string esc_s(const std::string& s) { return sh_escape_single(s); }

// ─────────────────────────── Универсальный health-check ──────────────────────
// curl -fsSL -m 5 [ -H "Host: <host_header>" ] <url>
bool check_url(const std::string& url, const std::string& host_header) {
    std::ostringstream cmd;
    cmd << "curl -fsSL -m 5 ";
    if (!host_header.empty())
        cmd << "-H 'Host: " << host_header << "' ";
    cmd << esc_s(url) << " >/dev/null 2>&1";
    return std::system(cmd.str().c_str()) == 0;
}

// URL для проверки main: cfg.health_url или fallback на локальный порт
static std::string main_health_url(const Config& cfg) {
    return cfg.health_url.empty()
        ? ("http://127.0.0.1:" + std::to_string(cfg.local_http_port) + "/")
        : cfg.health_url;
}

// ─────────────────────────── Одноразовый удалённый exec ──────────────────────
static int ssh_exec_one_host(const std::string& host, int port, const std::string& user,
                             const std::string& pass, const std::string& command, bool print_out = true) {
    ssh_session s = ssh_new();
    if (!s) { std::cerr << "❌ ssh_new()\n"; return -1; }

    ssh_options_set(s, SSH_OPTIONS_HOST, host.c_str());
    ssh_options_set(s, SSH_OPTIONS_USER, user.c_str());
    ssh_options_set(s, SSH_OPTIONS_PORT, &port);

    if (ssh_connect(s) != SSH_OK) {
        std::cerr << "❌ ssh_connect: " << ssh_get_error(s) << "\n";
        ssh_free(s);
        return -1;
    }
    if (ssh_userauth_password(s, nullptr, pass.c_str()) != SSH_AUTH_SUCCESS) {
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

// ─────────────────────────── Поиск зеркала по имени ──────────────────────────
static const Mirror* find_mirror(const Config& cfg, const std::string& name) {
    for (const auto& m : cfg.mirrors)
        if (m.name == name) return &m;
    return nullptr;
}

// ─────────────────────────── Супервизия оркестратора ─────────────────────────
static void supervise_orchestrator(const Config& cfg) {
    // systemctl is-active: 0 = active, иначе неактивен/ошибка
    const int rc = ssh_exec_one_host(cfg.proxy.host, cfg.proxy.ssh_port, cfg.proxy.user,
                                     cfg.proxy.pass, "systemctl is-active madbackuper-proxy",
                                     /*print_out=*/false);
    if (rc == 0) return; // оркестратор жив

    std::cout << "⚠️  Оркестратор на " << cfg.proxy.host << " не активен — перезапускаю\n";
    const int rrc = ssh_exec_one_host(cfg.proxy.host, cfg.proxy.ssh_port, cfg.proxy.user,
                                      cfg.proxy.pass, "systemctl restart madbackuper-proxy",
                                      /*print_out=*/true);
    if (rrc == 0) std::cout << "🔄 Оркестратор перезапущен\n";
    else          std::cerr << "❌ Не удалось перезапустить оркестратор на " << cfg.proxy.host << "\n";
}

// ─────────────────────────── Плановые бэкапы по расписанию ───────────────────
static std::string now_hhmm() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[6];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    return std::string(buf);
}

static void run_scheduled_backups(const Config& cfg, std::string& last_backup_date) {
    const std::string today_str = today();
    if (last_backup_date == today_str) return;   // сегодня уже запускали
    if (now_hhmm() < cfg.schedule_hhmm) return;  // ещё не наступило время

    last_backup_date = today_str;
    for (const auto& s : cfg.sites) {
        for (const auto& mn : s.mirrors) {
            if (!find_mirror(cfg, mn)) continue;
            const std::string cmd = "/usr/local/bin/madbackuper --site=" + s.name + " --mirror=" + mn;
            std::cout << "🗓 Плановый бэкап: " << cmd << "\n";
            const int rc = std::system(cmd.c_str());
            std::cout << (rc == 0 ? "✅ Бэкап OK: " : "❌ Бэкап завершился с кодом " + std::to_string(rc) + ": ")
                      << s.name << "/" << mn << "\n";
        }
    }
}

// ───────────────────────────── Демон-цикл (монитор) ──────────────────────────
int run_daemon_loop(const Config& cfg) {
    std::cout << "🌀 Демон-монитор запущен: health-check main/зеркал/proxy + плановые бэкапы\n";

    const int interval = cfg.health_interval_sec > 0 ? cfg.health_interval_sec : 60;

    // Состояние в памяти: последнее известное состояние каждого проверяемого узла
    bool main_known = false, main_last_ok = false;
    std::map<std::pair<std::string, std::string>, bool> mirror_last_ok;
    bool proxy_known = false, proxy_last_ok = false;
    std::string last_backup_date; // дата последнего запуска планового бэкапа

    while (true) {
        // 1) main
        {
            const bool ok = check_url(main_health_url(cfg), cfg.health_host_header);
            if (!main_known || ok != main_last_ok) {
                std::cout << (ok ? "🟢 main: OK" : "🔴 main: НЕДОСТУПЕН") << "\n";
                main_last_ok = ok;
                main_known = true;
            }
        }

        // 2) зеркала (по сайтам)
        for (const auto& s : cfg.sites) {
            for (const auto& mn : s.mirrors) {
                const Mirror* m = find_mirror(cfg, mn);
                if (!m) continue;
                const bool ok = check_url(m->health_url, s.server_name);
                const auto key = std::make_pair(s.name, mn);
                const auto it = mirror_last_ok.find(key);
                if (it == mirror_last_ok.end() || it->second != ok) {
                    std::cout << (ok ? "🟢" : "🔴") << " Сайт " << s.name << " / зеркало " << mn
                              << ": " << (ok ? "OK" : "НЕДОСТУПЕН") << "\n";
                    mirror_last_ok[key] = ok;
                }
            }
        }

        // 3) proxy + супервизия оркестратора
        if (cfg.has_proxy) {
            const bool ok = check_url(cfg.proxy.health_url, "");
            if (!proxy_known || ok != proxy_last_ok) {
                std::cout << (ok ? "🟢 proxy: OK" : "🔴 proxy: НЕДОСТУПЕН") << "\n";
                proxy_last_ok = ok;
                proxy_known = true;
            }
            supervise_orchestrator(cfg);
        }

        // 4) плановый бэкап (запуск самого себя в однократном режиме)
        run_scheduled_backups(cfg, last_backup_date);

        std::this_thread::sleep_for(std::chrono::seconds(interval));
    }
    // недостижимо
    // return 0;
}

// ─────────────────────────── Однократный отчёт (--status) ────────────────────
int status_report(const Config& cfg) {
    std::cout << "📊 Статус:\n";

    // main
    {
        const bool ok = check_url(main_health_url(cfg), cfg.health_host_header);
        std::cout << "main:            " << (ok ? "🟢 OK" : "🔴 НЕДОСТУПЕН") << "\n";
    }

    // зеркала (по сайтам)
    for (const auto& s : cfg.sites) {
        for (const auto& mn : s.mirrors) {
            const Mirror* m = find_mirror(cfg, mn);
            if (!m) {
                std::cout << s.name << "/" << mn << ":   ⚠️ зеркало не найдено в конфиге\n";
                continue;
            }
            const bool ok = check_url(m->health_url, s.server_name);
            std::cout << s.name << "/" << mn << ":   "
                      << (ok ? "🟢 OK" : "🔴 НЕДОСТУПЕН")
                      << " (" << m->remote_host << ":" << m->local_http_port << ")\n";
        }
    }

    // proxy
    if (cfg.has_proxy) {
        const bool ok = check_url(cfg.proxy.health_url, "");
        std::cout << "proxy:           " << (ok ? "🟢 OK" : "🔴 НЕДОСТУПЕН") << "\n";
    }

    return 0;
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

// ───────────────────────── Локальный systemd-юнит (main) ─────────────────────
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

// ───────────────────────── Публичные API установки демона ────────────────────
int daemon_install(const Config& cfg, const std::string& self_path)
{
    std::string exe = self_path.empty() ? readlink_self() : self_path;
    if (exe.empty()) { std::cerr << "❌ Не удалось определить путь к бинарю\n"; return 1; }

    int rc_local = install_local_service(exe);
    if (rc_local != 0) std::cerr << "⚠️  Проблема при установке локального юнита\n";

    if (cfg.has_proxy) {
        std::cout << "🛰 Устанавливаю оркестратор на proxy " << cfg.proxy.host << "\n";
        if (proxy_install) {
            const int rc_proxy = proxy_install(cfg);
            if (rc_proxy != 0) std::cerr << "⚠️  Проблема при установке оркестратора на " << cfg.proxy.host << "\n";
            else               std::cout << "✅ Оркестратор установлен на " << cfg.proxy.host << "\n";
        } else {
            std::cerr << "⚠️  Модуль оркестратора (proxy.cpp) не собран — пропускаю установку на " << cfg.proxy.host << "\n";
        }
    }

    std::cout << "✅ Установка демона завершена. Логи: journalctl -u madbackuper -f\n";
    return 0; // завершаем текущий процесс — дальше рулит systemd
}

int daemon_uninstall(const Config& cfg)
{
    int rc_local = uninstall_local_service();

    if (cfg.has_proxy) {
        if (proxy_uninstall) {
            const int rc_proxy = proxy_uninstall(cfg);
            if (rc_proxy != 0) std::cerr << "⚠️  Не удалось корректно убрать оркестратор на " << cfg.proxy.host << "\n";
        } else {
            std::cerr << "⚠️  Модуль оркестратора (proxy.cpp) не собран — пропускаю удаление на " << cfg.proxy.host << "\n";
        }
    }

    std::cout << "✅ Демон отключён локально. Для проверки: systemctl status madbackuper\n";
    return rc_local == 0 ? 0 : rc_local;
}

} // namespace mad