// src/modules/proxy.cpp
// Proxy-оркестратор: маршрутизация сайтов на живые бэкенды (mirror1..N, main).
// Единственный писатель маршрутизации — этот оркестратор (крутится на proxy-хосте, root).
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdlib>

#include <unistd.h>     // readlink, geteuid
#include <limits.h>

#include "mad/proxy.hpp"
#include "mad/core.hpp"
#include "mad/net.hpp"

namespace mad {

// ───────── Вспомогалки ─────────
static inline std::string esc_s(const std::string& s) { return sh_escape_single(s); }

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

// curl -fsSL -m 5 [ -H "Host: X" ] URL → system()==0
static bool check_url(const std::string& url, const std::string& host_header) {
    std::ostringstream cmd;
    cmd << "curl -fsSL -m 5 ";
    if (!host_header.empty()) cmd << "-H \"Host: " << host_header << "\" ";
    cmd << esc_s(url) << " >/dev/null 2>&1";
    return std::system(cmd.str().c_str()) == 0;
}

// http://host:port/path → host, port (порт по умолчанию 80)
static bool parse_http_url(const std::string& url, std::string& host, int& port) {
    std::string u = url;
    auto scheme = u.find("://");
    if (scheme == std::string::npos) return false;
    u = u.substr(scheme + 3);
    auto slash = u.find('/');
    if (slash != std::string::npos) u = u.substr(0, slash);
    auto colon = u.rfind(':');
    if (colon == std::string::npos) { host = u; port = 80; return !host.empty(); }
    host = u.substr(0, colon);
    try { port = std::stoi(u.substr(colon + 1)); } catch (...) { return false; }
    return !host.empty() && port > 0;
}

// ───────── SSH-сессия на proxy-хост ─────────
static ssh_session proxy_ssh_connect(const Config& cfg) {
    ssh_session s = ssh_new();
    if (!s) { std::cerr << "❌ ssh_new()\n"; return nullptr; }
    ssh_options_set(s, SSH_OPTIONS_HOST, cfg.proxy.host.c_str());
    ssh_options_set(s, SSH_OPTIONS_USER, cfg.proxy.user.c_str());
    ssh_options_set(s, SSH_OPTIONS_PORT, &cfg.proxy.ssh_port);
    if (ssh_connect(s) != SSH_OK) {
        std::cerr << "❌ ssh_connect(" << cfg.proxy.host << "): " << ssh_get_error(s) << "\n";
        ssh_free(s); return nullptr;
    }
    if (ssh_userauth_password(s, nullptr, cfg.proxy.pass.c_str()) != SSH_AUTH_SUCCESS) {
        std::cerr << "❌ ssh auth failed (" << cfg.proxy.host << "): " << ssh_get_error(s) << "\n";
        ssh_disconnect(s); ssh_free(s); return nullptr;
    }
    return s;
}

// ───────── Состояние бэкенда / сайта ─────────
struct BackendState {
    std::string name;      // "mirror1" | "main"
    std::string host;
    int port = 0;
    std::string url;       // health-check URL
    int fail_count = 0;
    int ok_count = 0;
    bool up = false;
    int fail_threshold = 3;
    int ok_threshold = 3;
};

struct SiteState {
    std::string active_backend;   // какой бэкенд сейчас прописан в конфиге маршрутизации
    bool all_down_warned = false;
    std::vector<BackendState> backends;
};

// ───────── Запись конфигов маршрутизации (локально, как root) ─────────
static int write_nginx_routing(const Site& S, const std::string& host, int port,
                               const std::vector<std::pair<std::string,int>>& backups) {
    std::ostringstream conf;
    conf << "upstream madbackuper_" << S.name << " {\n";
    conf << "    server " << host << ":" << port << ";\n";            // первый живой
    for (const auto& b : backups)
        conf << "    server " << b.first << ":" << b.second << " backup;\n"; // остальные живые — backup
    conf << "}\n";
    conf << "server {\n";
    conf << "    listen 80;\n";
    conf << "    server_name " << S.server_name << ";\n";
    conf << "    location / {\n";
    conf << "        proxy_pass http://madbackuper_" << S.name << ";\n";
    conf << "        proxy_set_header Host $host;\n";
    conf << "        proxy_set_header X-Real-IP $remote_addr;\n";
    conf << "        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n";
    conf << "        proxy_set_header X-Forwarded-Proto $scheme;\n";
    conf << "    }\n";
    conf << "}\n";

    const std::string path = "/etc/nginx/conf.d/madbackuper-" + S.name + ".conf";
    std::ofstream f(path);
    if (!f) { std::cerr << "❌ Не удалось записать " << path << "\n"; return -1; }
    f << conf.str();
    f.close();
    return run_local("nginx -t && systemctl reload nginx", /*echo=*/true);
}

static int write_apache_routing(const Site& S, const std::string& host, int port) {
    std::ostringstream conf;
    conf << "<VirtualHost *:80>\n";
    conf << "    ServerName " << S.server_name << "\n";
    conf << "    ProxyPreserveHost On\n";
    conf << "    ProxyPass / http://" << host << ":" << port << "/\n";
    conf << "    ProxyPassReverse / http://" << host << ":" << port << "/\n";
    conf << "</VirtualHost>\n";

    const std::string path = "/etc/apache2/sites-available/madbackuper-" + S.name + ".conf";
    std::ofstream f(path);
    if (!f) { std::cerr << "❌ Не удалось записать " << path << "\n"; return -1; }
    f << conf.str();
    f.close();

    // включить сайт, если ещё не включён
    run_local("a2query -s madbackuper-" + S.name + " >/dev/null 2>&1 || a2ensite madbackuper-" + S.name, true);
    return run_local("apache2ctl configtest && systemctl reload apache2", /*echo=*/true);
}

// ───────── Режим --proxy: бесконечный цикл оркестратора ─────────
int proxy_run(const Config& cfg) {
    std::cout << "🌀 Proxy-оркестратор запущен (server_type="
              << (cfg.proxy.server_type.empty() ? "nginx" : cfg.proxy.server_type) << ")\n";
    if (::geteuid() != 0)
        std::cerr << "⚠️ Оркестратор должен работать от root (сейчас uid=" << ::geteuid() << ")\n";

    const int interval = cfg.health_interval_sec > 0 ? cfg.health_interval_sec : 60;

    // Собираем состояние по сайтам один раз (бэкенды = [mirror1..N] + main)
    std::vector<SiteState> states;
    for (const auto& S : cfg.sites) {
        SiteState st;
        for (const auto& mname : S.mirrors) {
            const Mirror* M = nullptr;
            for (const auto& m : cfg.mirrors)
                if (m.name == mname) { M = &m; break; }
            if (!M) {
                std::cerr << "⚠️ Сайт " << S.name << ": зеркало «" << mname << "» не найдено в конфиге\n";
                continue;
            }
            BackendState b;
            b.name = mname;
            b.host = M->remote_host;
            b.port = M->local_http_port;
            b.url = M->health_url;
            b.fail_threshold = M->fail_threshold > 0 ? M->fail_threshold : 3;
            b.ok_threshold   = M->ok_threshold   > 0 ? M->ok_threshold   : 3;
            if (b.url.empty())
                std::cerr << "⚠️ Сайт " << S.name << ": у зеркала «" << mname
                          << "» пуст health_url — бэкенд будет считаться DOWN\n";
            st.backends.push_back(b);
        }
        // main — последний в приоритете
        if (cfg.main_health_url.empty()) {
            std::cerr << "⚠️ Сайт " << S.name << ": main_health_url пуст — main пропущен\n";
        } else {
            BackendState b;
            b.name = "main";
            b.url = cfg.main_health_url;
            b.fail_threshold = cfg.proxy.fail_threshold > 0 ? cfg.proxy.fail_threshold : 3;
            b.ok_threshold   = cfg.proxy.ok_threshold   > 0 ? cfg.proxy.ok_threshold   : 3;
            if (parse_http_url(cfg.main_health_url, b.host, b.port)) {
                st.backends.push_back(b);
            } else {
                std::cerr << "⚠️ Сайт " << S.name << ": main_health_url не парсится — main пропущен\n";
            }
        }
        if (st.backends.empty())
            std::cerr << "⚠️ Сайт " << S.name << ": нет ни одного бэкенда — пропускаю\n";
        states.push_back(st);
    }

    while (true) {
        for (size_t i = 0; i < cfg.sites.size(); ++i) {
            const Site& S = cfg.sites[i];
            SiteState& st = states[i];
            if (st.backends.empty()) continue;

            // health-check всех бэкендов
            for (auto& b : st.backends) {
                const bool ok = check_url(b.url, S.server_name);
                if (ok) { b.ok_count++; b.fail_count = 0; if (b.ok_count >= b.ok_threshold) b.up = true; }
                else    { b.fail_count++; b.ok_count = 0; if (b.fail_count >= b.fail_threshold) b.up = false; }
            }

            // активный бэкенд: форс-файл (если форс-бэкенд UP) → иначе первый UP в приоритете
            std::string active;
            {
                const std::string force_path = "/var/lib/madbackuper/" + S.name + ".force";
                std::string force_backend;
                std::ifstream f(force_path);
                if (f) { std::getline(f, force_backend); force_backend = trim(force_backend); }
                if (!force_backend.empty() && force_backend != "auto") {
                    for (const auto& b : st.backends)
                        if (b.name == force_backend && b.up) { active = b.name; break; }
                }
            }
            if (active.empty()) {
                for (const auto& b : st.backends)
                    if (b.up) { active = b.name; break; }
            }

            if (active.empty()) {
                if (!st.all_down_warned) {
                    std::cout << "⚠️ Сайт " << S.name << ": все бэкенды недоступны\n";
                    st.all_down_warned = true;
                }
                continue; // конфиг не трогаем
            }
            st.all_down_warned = false;

            if (active != st.active_backend) {
                const BackendState* ab = nullptr;
                for (const auto& b : st.backends) if (b.name == active) { ab = &b; break; }
                if (!ab) continue;

                std::vector<std::pair<std::string,int>> backups;
                for (const auto& b : st.backends)
                    if (b.up && b.name != active) backups.emplace_back(b.host, b.port);

                const bool apache = (cfg.proxy.server_type == "apache2");
                const int rc = apache ? write_apache_routing(S, ab->host, ab->port)
                                      : write_nginx_routing(S, ab->host, ab->port, backups);
                if (rc == 0) {
                    st.active_backend = active;
                    std::cout << "🔄 Сайт " << S.name << ": бэкенд → " << active
                              << " (" << ab->host << ":" << ab->port << ")\n";
                } else {
                    std::cerr << "❌ Сайт " << S.name << ": не удалось применить маршрутизацию на «"
                              << active << "»\n";
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(interval));
    }
    // недостижимо
    return 0;
}

// ───────── Установка оркестратора на proxy-хост (SSH) ─────────
int proxy_install(const Config& cfg) {
    if (cfg.proxy.host.empty()) { std::cerr << "❌ proxy.host не задан в конфиге\n"; return 1; }

    ssh_session s = proxy_ssh_connect(cfg);
    if (!s) return -1;

    // 1) проверка веб-сервера на proxy
    const std::string bin = (cfg.proxy.server_type == "apache2") ? "apache2ctl" : "nginx";
    if (ssh_exec(s, "command -v " + bin + " >/dev/null 2>&1", /*print_out=*/false) != 0) {
        std::cerr << "❌ На proxy не установлен " << bin << " (" << cfg.proxy.host << ")\n";
        ssh_disconnect(s); ssh_free(s); return -1;
    }

    // 2) SFTP бинаря → /tmp/madbackuper
    const std::string exe = readlink_self();
    if (exe.empty()) { std::cerr << "❌ Не удалось определить путь к бинарю\n"; ssh_disconnect(s); ssh_free(s); return 1; }

    int sftp_err = 0;
    sftp_session sf = sftp_new(s);
    if (!sf || sftp_init(sf) != SSH_OK) { if (sf) sftp_free(sf); ssh_disconnect(s); ssh_free(s); return -1; }
    if (sftp_upload_file_progress(s, sf, exe, "/tmp/madbackuper", "proxy-bin", &sftp_err, 0755) != 0) {
        std::cerr << "❌ Не удалось загрузить бинарь на " << cfg.proxy.host << " (SFTP)\n";
        sftp_free(sf); ssh_disconnect(s); ssh_free(s); return -1;
    }
    sftp_free(sf);

    // 2b) SFTP конфига → /tmp/madbackuper.conf (оркестратор читает /etc/madbackuper.conf на proxy)
    std::string local_cfg;
    if (::access("/etc/madbackuper.conf", R_OK) == 0) local_cfg = "/etc/madbackuper.conf";
    else if (::access("/root/madbackuper.conf", R_OK) == 0) local_cfg = "/root/madbackuper.conf";
    if (local_cfg.empty()) {
        std::cerr << "❌ Конфиг не найден (ни /etc/madbackuper.conf, ни /root/madbackuper.conf)\n";
        ssh_disconnect(s); ssh_free(s); return 1;
    }
    sftp_session sf3 = sftp_new(s);
    if (!sf3 || sftp_init(sf3) != SSH_OK) { if (sf3) sftp_free(sf3); ssh_disconnect(s); ssh_free(s); return -1; }
    if (sftp_upload_file_progress(s, sf3, local_cfg, "/tmp/madbackuper.conf", "proxy-conf", &sftp_err, 0600) != 0) {
        std::cerr << "❌ Не удалось загрузить конфиг на " << cfg.proxy.host << " (SFTP)\n";
        sftp_free(sf3); ssh_disconnect(s); ssh_free(s); return -1;
    }
    sftp_free(sf3);

    // 3) unit-файл → /tmp/madbackuper-proxy.service
    std::ostringstream unit;
    unit << R"([Unit]
Description=madbackuper proxy orchestrator
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/bin/madbackuper --proxy
Restart=always
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
)";
    const std::string local_unit = "/tmp/madbackuper-proxy.service";
    { std::ofstream u(local_unit); u << unit.str(); }

    sftp_session sf2 = sftp_new(s);
    if (!sf2 || sftp_init(sf2) != SSH_OK) { if (sf2) sftp_free(sf2); ssh_disconnect(s); ssh_free(s); return -1; }
    if (sftp_upload_file_progress(s, sf2, local_unit, "/tmp/madbackuper-proxy.service", "proxy-unit", &sftp_err, 0644) != 0) {
        std::cerr << "❌ Не удалось загрузить unit-файл на " << cfg.proxy.host << "\n";
        sftp_free(sf2); ssh_disconnect(s); ssh_free(s); return -1;
    }
    sftp_free(sf2);

    // 4) sudo: бинарь → /usr/local/bin, unit → /etc/systemd/system, reload/enable/restart
    const std::string pw = cfg.proxy.sudo_pass.empty() ? cfg.proxy.pass : cfg.proxy.sudo_pass;
    const std::string PW = esc_s(pw);
    {
        std::ostringstream cmd;
        cmd << "sh -lc 'set -e; "
            << "printf %s " << PW << " | sudo -S -p \"\" install -m 0755 /tmp/madbackuper /usr/local/bin/madbackuper; "
            << "printf %s " << PW << " | sudo -S -p \"\" mv /tmp/madbackuper.conf /etc/madbackuper.conf; "
            << "printf %s " << PW << " | sudo -S -p \"\" mv /tmp/madbackuper-proxy.service /etc/systemd/system/madbackuper-proxy.service; "
            << "printf %s " << PW << " | sudo -S -p \"\" systemctl daemon-reload; "
            << "printf %s " << PW << " | sudo -S -p \"\" systemctl enable madbackuper-proxy.service; "
            << "printf %s " << PW << " | sudo -S -p \"\" systemctl restart madbackuper-proxy.service"
            << "'";
        if (ssh_exec(s, cmd.str(), /*print_out=*/true) != 0) {
            std::cerr << "❌ Не удалось зарегистрировать/запустить proxy unit на " << cfg.proxy.host << "\n";
            ssh_disconnect(s); ssh_free(s); return -1;
        }
    }

    ssh_disconnect(s); ssh_free(s);
    std::cout << "✅ Proxy-оркестратор установлен на " << cfg.proxy.host
              << " (systemctl status madbackuper-proxy)\n";
    return 0;
}

// ───────── Удаление оркестратора с proxy-хоста ─────────
int proxy_uninstall(const Config& cfg) {
    if (cfg.proxy.host.empty()) { std::cerr << "❌ proxy.host не задан в конфиге\n"; return 1; }

    ssh_session s = proxy_ssh_connect(cfg);
    if (!s) return -1;

    const std::string pw = cfg.proxy.sudo_pass.empty() ? cfg.proxy.pass : cfg.proxy.sudo_pass;
    const std::string PW = esc_s(pw);
    const std::string conf_glob = (cfg.proxy.server_type == "apache2")
        ? "/etc/apache2/sites-available/madbackuper-*.conf /etc/apache2/sites-enabled/madbackuper-*.conf"
        : "/etc/nginx/conf.d/madbackuper-*.conf";

    std::ostringstream cmd;
    cmd << "sh -lc 'set -e; "
        << "printf %s " << PW << " | sudo -S -p \"\" systemctl stop madbackuper-proxy.service >/dev/null 2>&1 || true; "
        << "printf %s " << PW << " | sudo -S -p \"\" systemctl disable madbackuper-proxy.service >/dev/null 2>&1 || true; "
        << "printf %s " << PW << " | sudo -S -p \"\" rm -f /etc/systemd/system/madbackuper-proxy.service; "
        << "printf %s " << PW << " | sudo -S -p \"\" rm -f /usr/local/bin/madbackuper; "
        << "printf %s " << PW << " | sudo -S -p \"\" rm -f " << conf_glob << "; "
        << "printf %s " << PW << " | sudo -S -p \"\" systemctl daemon-reload"
        << "'";
    const int rc = ssh_exec(s, cmd.str(), /*print_out=*/true);
    ssh_disconnect(s); ssh_free(s);
    if (rc == 0) std::cout << "✅ Proxy-оркестратор удалён с " << cfg.proxy.host << "\n";
    return rc;
}

// ───────── Ручной форс бэкенда (--switch=SITE:BACKEND) ─────────
int proxy_switch(const Config& cfg, const std::string& site_name, const std::string& backend) {
    if (cfg.proxy.host.empty()) { std::cerr << "❌ proxy.host не задан в конфиге\n"; return 1; }

    if (backend != "auto" && backend != "main") {
        bool found = false;
        for (const auto& m : cfg.mirrors)
            if (m.name == backend) { found = true; break; }
        if (!found) { std::cerr << "❌ Неизвестный бэкенд: " << backend << "\n"; return 1; }
    }

    ssh_session s = proxy_ssh_connect(cfg);
    if (!s) return -1;

    const std::string pw = cfg.proxy.sudo_pass.empty() ? cfg.proxy.pass : cfg.proxy.sudo_pass;
    const std::string PW = esc_s(pw);
    const std::string site = esc_s(site_name);

    std::ostringstream cmd;
    cmd << "sh -lc 'set -e; ";
    if (backend == "auto") {
        cmd << "printf %s " << PW << " | sudo -S -p \"\" rm -f /var/lib/madbackuper/" << site << ".force";
    } else {
        const std::string b = esc_s(backend);
        cmd << "printf %s " << PW << " | sudo -S -p \"\" mkdir -p /var/lib/madbackuper; "
            << "printf %s " << PW << " | sudo -S -p \"\" sh -c \"echo " << b
            << " > /var/lib/madbackuper/" << site << ".force\"";
    }
    cmd << "'";

    const int rc = ssh_exec(s, cmd.str(), /*print_out=*/true);
    ssh_disconnect(s); ssh_free(s);
    if (rc == 0) {
        if (backend == "auto")
            std::cout << "✅ Сайт " << site_name << ": форс снят (авто-режим)\n";
        else
            std::cout << "✅ Сайт " << site_name << ": форс бэкенда → " << backend << "\n";
    }
    return rc;
}

} // namespace mad