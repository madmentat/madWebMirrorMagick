#include <string>
#include <cstdint>
#include <filesystem>
#include <chrono>
#include <iostream>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <type_traits>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

#include "mad/core.hpp"

namespace mad {

// ───────── Константы (определения extern из core.hpp) ─────────
const char* CFG_PATH_PRIMARY         = "/etc/madbackuper.conf";
const char* CFG_PATH_FALLBACK        = "/root/madbackuper.conf";
const char* DEFAULT_TARGET           = "nginx";      // или "apache2"
const int   DEFAULT_SSH_PORT         = 22;
const int   DEFAULT_LOCAL_HTTP_PORT  = 8081;
const int   DEFAULT_LOCAL_HTTPS_PORT = 0;
const bool  DEFAULT_SWITCH_TO_LOCAL  = true;
const std::string DEFAULT_PHP_VERSION = "8.3";
// NEW: дефолт для health-check интервала
const int   DEFAULT_HEALTH_INTERVAL_SEC = 60;

// ───────── base64 encoder ─────────
static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string &in) {
    std::string out;
    size_t i = 0;
    unsigned char a3[3]{};
    unsigned char a4[4]{};
    for (unsigned char c : in) {
        a3[i++] = c;
        if (i == 3) {
            a4[0] = (a3[0] & 0xfc) >> 2;
            a4[1] = ((a3[0] & 0x03) << 4) + ((a3[1] & 0xf0) >> 4);
            a4[2] = ((a3[1] & 0x0f) << 2) + ((a3[2] & 0xc0) >> 6);
            a4[3] = a3[2] & 0x3f;
            for (int j = 0; j < 4; j++) out.push_back(B64_TABLE[a4[j]]);
            i = 0;
        }
    }
    if (i) {
        for (size_t j = i; j < 3; j++) a3[j] = '\0';
        a4[0] = (a3[0] & 0xfc) >> 2;
        a4[1] = ((a3[0] & 0x03) << 4) + ((a3[1] & 0xf0) >> 4);
        a4[2] = ((a3[1] & 0x0f) << 2) + ((a3[2] & 0xc0) >> 6);
        a4[3] = a3[2] & 0x3f;
        for (size_t j = 0; j < i + 1; j++) out.push_back(B64_TABLE[a4[j]]);
        while ((i++ < 3)) out.push_back('=');
    }
    return out;
}

// ───────── Вспомогалки ─────────
std::string trim(const std::string& s) {
    auto l = s.find_first_not_of(" \t\r\n");
    auto r = s.find_last_not_of(" \t\r\n");
    if (l == std::string::npos) return {};
    return s.substr(l, r - l + 1);
}

std::string today() {
    char buf[16];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
}

std::string human_size(uint64_t bytes) {
    const char* u[] = {"B","KB","MB","GB","TB","PB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; ++i; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
    return buf;
}

// В header объявлено с std::filesystem::path — соблюдаем сигнатуру
uint64_t dir_size_bytes(const std::filesystem::path& root) {
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; ++it) {
        if (ec) { ec.clear(); continue; }
        std::error_code ec2;
        if (fs::is_regular_file(*it, ec2)) total += fs::file_size(*it, ec2);
    }
    return total;
}

bool has_command(const char* name) {
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// ───────── Конфиг I/O ─────────
void write_default_config(const std::string& path) {
    std::ofstream out(path);
    out
    << "# madbackuper.conf — секционный формат\n"
    << "# [main] — параметры main-сервера (origin)\n"
    << "[main]\n"
    << "health_url=http://127.0.0.1:80/\n"
    << "main_health_url=\n"
    << "health_interval_sec=" << DEFAULT_HEALTH_INTERVAL_SEC << "\n"
    << "schedule_hhmm=04:00\n"
    << "\n"
    << "# [site:имя] — сайт на main; mirrors — зеркала в порядке приоритета\n"
    << "[site:site1]\n"
    << "server_name=example.com\n"
    << "local_site_dir=/webserver/example.com\n"
    << "db_user=example\n"
    << "db_pass=XXX\n"
    << "db_name=example\n"
    << "mirrors=mirror1\n"
    << "\n"
    << "# [mirror:имя] — зеркало (копия сайта), target_server: nginx|apache2\n"
    << "[mirror:mirror1]\n"
    << "remote_host=192.168.88.202\n"
    << "ssh_port=" << DEFAULT_SSH_PORT << "\n"
    << "remote_user=madmentat\n"
    << "remote_pass=XXX\n"
    << "remote_sudo_pass=\n"
    << "remote_root_pass=XXX\n"
    << "target_server=" << DEFAULT_TARGET << "\n"
    << "remote_site_dir=/webserver/mirror_example.com\n"
    << "remote_backup_base=/webserver/.backup\n"
    << "local_http_port=" << DEFAULT_LOCAL_HTTP_PORT << "\n"
    << "local_https_port=" << DEFAULT_LOCAL_HTTPS_PORT << "\n"
    << "php_version=" << DEFAULT_PHP_VERSION << "\n"
    << "php_fpm_sock=\n"
    << "ssl_cert=\n"
    << "ssl_key=\n"
    << "health_url=http://192.168.88.202:8081/\n"
    << "fail_threshold=3\n"
    << "ok_threshold=3\n"
    << "\n"
    << "# [proxy] — отдельный proxy-сервер (публичный фронт, оркестратор)\n"
    << "# [proxy]\n"
    << "# host=192.168.88.106\n"
    << "# ssh_port=22\n"
    << "# user=madtest\n"
    << "# pass=XXX\n"
    << "# sudo_pass=\n"
    << "# server_type=nginx\n"
    << "# health_url=http://192.168.88.106/\n"
    << "# fail_threshold=3\n"
    << "# ok_threshold=3\n";
    out.close();
    chmod(path.c_str(), 0600);
}

// ───────── Вывод сущностей (sites/mirrors) из плоских полей и обратно ─────────
static void derive_entities(Config& cfg) {
    // 1. Если mirrors пуст — создаём Mirror из плоских полей (сначала зеркало,
    //    чтобы Site ниже мог сослаться на mirrors[0].name)
    if (cfg.mirrors.empty()) {
        Mirror m;
        m.name = "mirror1";
        m.remote_host = cfg.remote_host;
        m.ssh_port = cfg.ssh_port;
        m.remote_user = cfg.remote_user;
        m.remote_pass = cfg.remote_pass;
        m.remote_sudo_pass = cfg.remote_sudo_pass;
        m.remote_root_pass = cfg.remote_root_pass;
        m.target_server = cfg.target_server;
        m.remote_site_dir = cfg.remote_site_dir;
        m.remote_backup_base = cfg.remote_backup_base;
        m.local_http_port = cfg.local_http_port;
        m.local_https_port = cfg.local_https_port;
        m.php_version = cfg.php_version;
        m.php_fpm_sock = cfg.php_fpm_sock;
        m.ssl_cert = cfg.ssl_cert;
        m.ssl_key = cfg.ssl_key;
        m.health_url = "http://" + cfg.remote_host + ":" + std::to_string(cfg.local_http_port) + "/";
        cfg.mirrors.push_back(m);
    }
    // 2. Если sites пуст — создаём Site из плоских полей
    if (cfg.sites.empty()) {
        Site s;
        s.name = "site1";
        s.server_name = cfg.server_name;
        s.local_site_dir = cfg.local_site_dir;
        s.db_user = cfg.db_user;
        s.db_pass = cfg.db_pass;
        s.db_name = cfg.db_name;
        if (!cfg.mirrors.empty()) s.mirrors.push_back(cfg.mirrors[0].name);
        cfg.sites.push_back(s);
    }
    // 3. Секционный формат (или уже выведенные сущности) — копируем sites[0]/mirrors[0] в плоские поля
    if (!cfg.sites.empty() && !cfg.mirrors.empty()) {
        const Site& s = cfg.sites[0];
        cfg.server_name = s.server_name;
        cfg.local_site_dir = s.local_site_dir;
        cfg.db_user = s.db_user;
        cfg.db_pass = s.db_pass;
        cfg.db_name = s.db_name;
        const Mirror& m = cfg.mirrors[0];
        cfg.remote_host = m.remote_host;
        cfg.ssh_port = m.ssh_port;
        cfg.remote_user = m.remote_user;
        cfg.remote_pass = m.remote_pass;
        cfg.remote_sudo_pass = m.remote_sudo_pass;
        cfg.remote_root_pass = m.remote_root_pass;
        cfg.target_server = m.target_server;
        cfg.remote_site_dir = m.remote_site_dir;
        cfg.remote_backup_base = m.remote_backup_base;
        cfg.local_http_port = m.local_http_port;
        cfg.local_https_port = m.local_https_port;
        cfg.php_version = m.php_version;
        cfg.php_fpm_sock = m.php_fpm_sock;
        cfg.ssl_cert = m.ssl_cert;
        cfg.ssl_key = m.ssl_key;
    }
    // proxy_target/switch_to_local оставляем как есть (устаревшие, не используются новой логикой)
}

void load_kv_file(const std::string& path, Config& cfg) {
    std::ifstream in(path);
    if (!in) return;

    // Предупреждение о слишком открытых правах на конфиг (секреты в нём)
    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && (st.st_mode & 0077) != 0) {
        std::cerr << "⚠️ Конфиг " << path
                  << " доступен на чтение другим пользователям — выполни chmod 600\n";
    }

    // Читаем файл целиком построчно
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);

    // Определяем формат: есть ли секции вида [..]
    bool has_sections = false;
    for (const auto& l : lines) {
        std::string t = trim(l);
        if (!t.empty() && t[0] == '[') { has_sections = true; break; }
    }
    cfg.section_format = has_sections;

    if (!has_sections) {
        // ── старый плоский формат ──
        for (const auto& l : lines) {
            std::string t = trim(l);
            if (t.empty() || t[0]=='#' || t[0]==';') continue;
            auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            auto k = trim(t.substr(0, eq));
            auto v = trim(t.substr(eq+1));

            if      (k=="target_server")        cfg.target_server = v;
            else if (k=="remote_host")          cfg.remote_host = v;
            else if (k=="ssh_port")             cfg.ssh_port = std::stoi(v);
            else if (k=="remote_user")          cfg.remote_user = v;
            else if (k=="remote_pass")          cfg.remote_pass = v;
            else if (k=="remote_root_pass")     cfg.remote_root_pass = v;
            else if (k=="remote_sudo_pass")     cfg.remote_sudo_pass = v;
            else if (k=="local_site_dir")       cfg.local_site_dir = v;
            else if (k=="remote_site_dir")      cfg.remote_site_dir = v;
            else if (k=="remote_backup_base")   cfg.remote_backup_base = v;
            else if (k=="server_name")          cfg.server_name = v;
            else if (k=="php_version")          cfg.php_version = v;
            else if (k=="php_fpm_sock")         cfg.php_fpm_sock = v;
            else if (k=="db_user")              cfg.db_user = v;
            else if (k=="db_pass")              cfg.db_pass = v;
            else if (k=="db_name")              cfg.db_name = v;
            else if (k=="proxy_target")         cfg.proxy_target = v;
            else if (k=="local_http_port")      cfg.local_http_port = std::stoi(v);
            else if (k=="local_https_port")     cfg.local_https_port = std::stoi(v);
            else if (k=="health_url")           cfg.health_url = v;
            else if (k=="health_host_header")   cfg.health_host_header = v;
            else if (k=="switch_to_local")      cfg.switch_to_local = (v=="true"||v=="1"||v=="yes");
            else if (k=="ssl_cert")             cfg.ssl_cert = v;
            else if (k=="ssl_key")              cfg.ssl_key = v;
            else if (k=="health_interval_sec")  cfg.health_interval_sec = std::stoi(v);
            else if (k=="schedule_hhmm")        cfg.schedule_hhmm = v;
        }
        derive_entities(cfg);
        return;
    }

    // ── секционный формат ──
    std::string cur; // текущая секция: "main" | "site:имя" | "mirror:имя" | "proxy"
    for (const auto& l : lines) {
        std::string t = trim(l);
        if (t.empty() || t[0]=='#' || t[0]==';') continue;
        if (t[0] == '[') {
            if (t.size() < 2 || t.back() != ']') {
                std::cerr << "⚠️ Некорректная секция: " << t << "\n";
                cur.clear();
                continue;
            }
            cur = trim(t.substr(1, t.size()-2));
            continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        auto k = trim(t.substr(0, eq));
        auto v = trim(t.substr(eq+1));
        if (cur.empty()) {
            std::cerr << "⚠️ Ключ вне секции: " << k << "\n";
            continue;
        }

        if (cur == "main") {
            if      (k=="health_url")           cfg.health_url = v;
            else if (k=="main_health_url")      cfg.main_health_url = v;
            else if (k=="health_interval_sec")  cfg.health_interval_sec = std::stoi(v);
            else if (k=="schedule_hhmm")        cfg.schedule_hhmm = v;
            else if (k=="health_host_header")   cfg.health_host_header = v;
            else std::cerr << "⚠️ Неизвестный ключ " << k << " в секции " << cur << "\n";
        }
        else if (cur.rfind("site:", 0) == 0) {
            std::string sname = trim(cur.substr(5));
            auto it = std::find_if(cfg.sites.begin(), cfg.sites.end(),
                                   [&](const Site& s){ return s.name == sname; });
            if (it == cfg.sites.end()) {
                cfg.sites.push_back(Site{});
                it = cfg.sites.end() - 1;
                it->name = sname;
            }
            Site& s = *it;
            if      (k=="server_name")      s.server_name = v;
            else if (k=="local_site_dir")   s.local_site_dir = v;
            else if (k=="db_user")          s.db_user = v;
            else if (k=="db_pass")          s.db_pass = v;
            else if (k=="db_name")          s.db_name = v;
            else if (k=="mirrors") {
                s.mirrors.clear();
                std::stringstream ss(v);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    std::string mn = trim(item);
                    if (!mn.empty()) s.mirrors.push_back(mn);
                }
            }
            else std::cerr << "⚠️ Неизвестный ключ " << k << " в секции " << cur << "\n";
        }
        else if (cur.rfind("mirror:", 0) == 0) {
            std::string mname = trim(cur.substr(7));
            auto it = std::find_if(cfg.mirrors.begin(), cfg.mirrors.end(),
                                   [&](const Mirror& m){ return m.name == mname; });
            if (it == cfg.mirrors.end()) {
                cfg.mirrors.push_back(Mirror{});
                it = cfg.mirrors.end() - 1;
                it->name = mname;
            }
            Mirror& m = *it;
            if      (k=="remote_host")        m.remote_host = v;
            else if (k=="ssh_port")           m.ssh_port = std::stoi(v);
            else if (k=="remote_user")        m.remote_user = v;
            else if (k=="remote_pass")        m.remote_pass = v;
            else if (k=="remote_sudo_pass")   m.remote_sudo_pass = v;
            else if (k=="remote_root_pass")   m.remote_root_pass = v;
            else if (k=="target_server")      m.target_server = v;
            else if (k=="remote_site_dir")    m.remote_site_dir = v;
            else if (k=="remote_backup_base") m.remote_backup_base = v;
            else if (k=="local_http_port")    m.local_http_port = std::stoi(v);
            else if (k=="local_https_port")   m.local_https_port = std::stoi(v);
            else if (k=="php_version")        m.php_version = v;
            else if (k=="php_fpm_sock")       m.php_fpm_sock = v;
            else if (k=="ssl_cert")           m.ssl_cert = v;
            else if (k=="ssl_key")            m.ssl_key = v;
            else if (k=="health_url")         m.health_url = v;
            else if (k=="fail_threshold")     m.fail_threshold = std::stoi(v);
            else if (k=="ok_threshold")       m.ok_threshold = std::stoi(v);
            else std::cerr << "⚠️ Неизвестный ключ " << k << " в секции " << cur << "\n";
        }
        else if (cur == "proxy") {
            if      (k=="host")           cfg.proxy.host = v;
            else if (k=="ssh_port")       cfg.proxy.ssh_port = std::stoi(v);
            else if (k=="user")           cfg.proxy.user = v;
            else if (k=="pass")           cfg.proxy.pass = v;
            else if (k=="sudo_pass")      cfg.proxy.sudo_pass = v;
            else if (k=="server_type")    cfg.proxy.server_type = v;
            else if (k=="health_url")     cfg.proxy.health_url = v;
            else if (k=="fail_threshold") cfg.proxy.fail_threshold = std::stoi(v);
            else if (k=="ok_threshold")   cfg.proxy.ok_threshold = std::stoi(v);
            else std::cerr << "⚠️ Неизвестный ключ " << k << " в секции " << cur << "\n";
            cfg.has_proxy = true;
        }
        else {
            std::cerr << "⚠️ Неизвестная секция " << cur << "\n";
        }
    }

    if (cfg.sites.empty()) {
        std::cerr << "❌ В конфиге нет ни одной секции [site:...]\n";
    }
    derive_entities(cfg);
}

// внутренний парсер HH:MM (для --at=)
static bool parse_hhmm(const std::string& s, int& hh, int& mm) {
    if (s.size()!=5 || s[2]!=':') return false;
    try { hh=std::stoi(s.substr(0,2)); mm=std::stoi(s.substr(3,2)); } catch(...) { return false; }
    return (0<=hh && hh<24) && (0<=mm && mm<60);
}

// «расширенная» версия (с поддержкой daemon и --at)
void apply_cli_kv(int argc, char** argv, Config& cfg,
                  bool& daemon_mode, int& alarm_h, int& alarm_m) {
    auto eat = [&](const std::string& arg, const char* key, auto& dst) {
        std::string p = std::string("--") + key + "=";
        if (arg.rfind(p, 0) == 0) {
            std::string val = arg.substr(p.size());
            if constexpr(std::is_same_v<decltype(dst), int&>)  dst = std::stoi(val);
            else if constexpr(std::is_same_v<decltype(dst), bool&>) dst = (val=="true"||val=="1"||val=="yes");
            else dst = val;
            return true;
        }
        return false;
    };

    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        // key=value
        if (eat(a, "target-server",        cfg.target_server))        continue;
        if (eat(a, "remote-host",          cfg.remote_host))          continue;
        if (eat(a, "ssh-port",             cfg.ssh_port))             continue;
        if (eat(a, "remote-user",          cfg.remote_user))          continue;
        if (eat(a, "remote-pass",          cfg.remote_pass))          continue;
        if (eat(a, "remote-root-pass",     cfg.remote_root_pass))     continue;
        if (eat(a, "remote-sudo-pass",     cfg.remote_sudo_pass))     continue;
        if (eat(a, "local-site-dir",       cfg.local_site_dir))       continue;
        if (eat(a, "remote-site-dir",      cfg.remote_site_dir))      continue;
        if (eat(a, "remote-backup-base",   cfg.remote_backup_base))   continue;
        if (eat(a, "server-name",          cfg.server_name))          continue;
        if (eat(a, "php-version",          cfg.php_version))          continue;
        if (eat(a, "php-fpm-sock",         cfg.php_fpm_sock))         continue;
        if (eat(a, "db-user",              cfg.db_user))              continue;
        if (eat(a, "db-pass",              cfg.db_pass))              continue;
        if (eat(a, "db-name",              cfg.db_name))              continue;
        if (eat(a, "proxy-target",         cfg.proxy_target))         continue;
        if (eat(a, "local-http-port",      cfg.local_http_port))      continue;
        if (eat(a, "local-https-port",     cfg.local_https_port))     continue;
        if (eat(a, "health-url",           cfg.health_url))           continue;
        if (eat(a, "health-host-header",   cfg.health_host_header))   continue;
        if (eat(a, "switch-to-local",      cfg.switch_to_local))      continue;
        if (eat(a, "ssl-cert",             cfg.ssl_cert))             continue;
        if (eat(a, "ssl-key",              cfg.ssl_key))              continue;
        if (eat(a, "health-interval-sec",  cfg.health_interval_sec))  continue;
        if (eat(a, "schedule",             cfg.schedule_hhmm))        continue;

        // новые ключи новой архитектуры (пишут в плоские поля / все сущности)
        {
            const std::string p_main = "--main-health-url=";
            if (a.rfind(p_main, 0) == 0) { cfg.main_health_url = a.substr(p_main.size()); continue; }
        }
        {
            const std::string p_fail = "--fail-threshold=";
            if (a.rfind(p_fail, 0) == 0) {
                int v = std::stoi(a.substr(p_fail.size()));
                for (auto& m : cfg.mirrors) m.fail_threshold = v;
                cfg.proxy.fail_threshold = v;
                continue;
            }
        }
        {
            const std::string p_ok = "--ok-threshold=";
            if (a.rfind(p_ok, 0) == 0) {
                int v = std::stoi(a.substr(p_ok.size()));
                for (auto& m : cfg.mirrors) m.ok_threshold = v;
                cfg.proxy.ok_threshold = v;
                continue;
            }
        }

        // флаги
        if (a=="--skip-tar")    { cfg.skip_tar = true;    continue; }
        if (a=="--skip-sql")    { cfg.skip_sql = true;    continue; }
        if (a=="--skip-upload") { cfg.skip_upload = true; continue; }
        if (a=="--daemon")      { daemon_mode = true;     continue; }
        if (a.rfind("--at=",0)==0){
            std::string v = a.substr(5);
            if (!parse_hhmm(v, alarm_h, alarm_m)) {
                std::cerr << "❌ Неверный формат --at=HH:MM\n";
                std::exit(1);
            }
            continue;
        }
    }
}

// «короткая» версия (совместимость с вызовами без демона)
void apply_cli_kv(int argc, char** argv, Config& cfg) {
    bool dummy_daemon = false; int dummy_h=0, dummy_m=0;
    apply_cli_kv(argc, argv, cfg, dummy_daemon, dummy_h, dummy_m);
}

// ───────── Проверка параметров ─────────
bool validate(const Config& c, std::string& err) {
    auto notEmpty = [&](const std::string& v, const char* name)->bool {
        if (v.empty()) { err = std::string("Параметр пуст: ") + name; return false; }
        return true;
    };
    if (!(c.target_server=="nginx" || c.target_server=="apache2")) { err = "target_server: nginx|apache2"; return false; }
    if (!notEmpty(c.remote_host, "remote_host")) return false;
    if (!notEmpty(c.remote_user, "remote_user")) return false;
    if (!notEmpty(c.remote_pass, "remote_pass")) return false;
    if (!notEmpty(c.local_site_dir, "local_site_dir")) return false;
    if (!notEmpty(c.remote_site_dir, "remote_site_dir")) return false;
    if (!notEmpty(c.remote_backup_base, "remote_backup_base")) return false;
    if (!notEmpty(c.server_name, "server_name")) return false;
    if (c.php_fpm_sock.empty() && !notEmpty(c.php_version, "php_version")) return false;
    if (!notEmpty(c.db_user, "db_user")) return false;
    if (!notEmpty(c.db_pass, "db_pass")) return false;
    if (!notEmpty(c.db_name, "db_name")) return false;
    if (!c.section_format && !notEmpty(c.proxy_target, "proxy_target")) return false;
    if (c.remote_pass == "XXX" || c.remote_sudo_pass == "XXX" || c.remote_root_pass == "XXX" || c.db_pass == "XXX") {
        err = "Плейсхолдер XXX в пароле — укажите реальный пароль в конфиге";
        return false;
    }
    if (c.local_http_port <= 0 || c.local_http_port == 80 || c.local_http_port == 443) { err = "local_http_port >0 и не 80/443"; return false; }
    if (c.local_https_port > 0) {
        if (!notEmpty(c.ssl_cert, "ssl_cert")) return false;
        if (!notEmpty(c.ssl_key, "ssl_key")) return false;
        if (c.local_https_port == 80 || c.local_https_port == 443 || c.local_https_port == c.local_http_port) {
            err = "local_https_port >0, не 80/443 и ≠ local_http_port"; return false;
        }
    }

    // ── Новые правила (multi-site / multi-mirror / proxy) ──
    if (c.sites.empty()) { err = "sites пуст — нет ни одного сайта"; return false; }
    for (const auto& s : c.sites) {
        if (s.server_name.empty())    { err = "site '" + s.name + "': server_name пуст"; return false; }
        if (s.local_site_dir.empty()) { err = "site '" + s.name + "': local_site_dir пуст"; return false; }
        if (s.db_user.empty())        { err = "site '" + s.name + "': db_user пуст"; return false; }
        if (s.db_pass.empty())        { err = "site '" + s.name + "': db_pass пуст"; return false; }
        if (s.db_pass == "XXX")       { err = "site '" + s.name + "': плейсхолдер XXX в db_pass — укажите реальный пароль"; return false; }
        if (s.db_name.empty())        { err = "site '" + s.name + "': db_name пуст"; return false; }
        if (s.mirrors.empty())        { err = "site '" + s.name + "': mirrors пуст — укажите хотя бы одно зеркало"; return false; }
        for (const auto& mn : s.mirrors) {
            bool found = false;
            for (const auto& m : c.mirrors) if (m.name == mn) { found = true; break; }
            if (!found) { err = "site '" + s.name + "': зеркало '" + mn + "' не найдено в [mirror:...]"; return false; }
        }
    }
    if (c.mirrors.empty()) { err = "mirrors пуст — нет ни одного зеркала"; return false; }
    for (const auto& m : c.mirrors) {
        if (m.remote_host.empty())        { err = "mirror '" + m.name + "': remote_host пуст"; return false; }
        if (m.remote_user.empty())        { err = "mirror '" + m.name + "': remote_user пуст"; return false; }
        if (m.remote_pass.empty())        { err = "mirror '" + m.name + "': remote_pass пуст"; return false; }
        if (m.remote_pass == "XXX")       { err = "mirror '" + m.name + "': плейсхолдер XXX в remote_pass — укажите реальный пароль"; return false; }
        if (!(m.target_server=="nginx" || m.target_server=="apache2")) { err = "mirror '" + m.name + "': target_server: nginx|apache2"; return false; }
        if (m.remote_site_dir.empty())    { err = "mirror '" + m.name + "': remote_site_dir пуст"; return false; }
        if (m.remote_backup_base.empty()) { err = "mirror '" + m.name + "': remote_backup_base пуст"; return false; }
        if (m.local_http_port <= 0 || m.local_http_port == 80 || m.local_http_port == 443) { err = "mirror '" + m.name + "': local_http_port >0 и не 80/443"; return false; }
        if (m.fail_threshold < 1) { err = "mirror '" + m.name + "': fail_threshold ≥ 1"; return false; }
        if (m.ok_threshold < 1)   { err = "mirror '" + m.name + "': ok_threshold ≥ 1"; return false; }
    }
    if (c.has_proxy) {
        if (c.proxy.host.empty())        { err = "proxy: host пуст"; return false; }
        if (c.proxy.user.empty())        { err = "proxy: user пуст"; return false; }
        if (c.proxy.pass.empty())        { err = "proxy: pass пуст"; return false; }
        if (c.proxy.pass == "XXX")       { err = "proxy: плейсхолдер XXX в pass — укажите реальный пароль"; return false; }
        if (!(c.proxy.server_type=="nginx" || c.proxy.server_type=="apache2")) { err = "proxy: server_type: nginx|apache2"; return false; }
        if (c.proxy.health_url.empty())  { err = "proxy: health_url пуст"; return false; }
        if (c.proxy.fail_threshold < 1)  { err = "proxy: fail_threshold ≥ 1"; return false; }
        if (c.proxy.ok_threshold < 1)    { err = "proxy: ok_threshold ≥ 1"; return false; }
    }
    if (c.main_health_url.empty()) {
        std::cerr << "⚠️ main_health_url пуст — proxy/зеркала не смогут проверить main извне (NAT).\n";
    }
    return true;
}

// ───────────────── run_local / run_with_spinner ─────────────────
int run_local(const std::string& cmd, bool echo) {
    if (echo) {
        std::cout << "➜ " << cmd << std::endl;
    }
    // std::system возвращает код завершения шелла; для простоты вернём его как есть
    return std::system(cmd.c_str());
}

// Если где-то остались вызовы "со спиннером" — пусть просто делегирует в run_local
int run_with_spinner(const std::string& cmd, const std::string& /*label*/) {
    return run_local(cmd, /*echo=*/true);
}



} // namespace mad
