#include <string>
#include <cstdint>
#include <filesystem>
#include <chrono>
#include <iostream>
#include <vector>
#include <fstream>
#include <type_traits>
#include <cstdlib>
#include <cstdio>
#include <ctime>

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
static const char CFG_HEADER[] =
R"(# madbackuper.conf
# ------------------------------------------------------------
# Конфигурация для madbackuper (автогенерируется при первом запуске).
# Все параметры можно переопределить CLI-ключами.
# ------------------------------------------------------------
# Основные параметры:
#   target_server=nginx            # допустимо: nginx | apache2
#   remote_host=192.168.88.202
#   ssh_port=22
#   remote_user=madmentat
#   remote_pass=XXX           # пароль для SSH-подключения пользователя
#   remote_sudo_pass=              # пароль, который спрашивает sudo; если пусто — берётся remote_pass
#   remote_root_pass=              # пароль для root (на будущее)
# Пути:
#   local_site_dir=/webserver/madmentat.ru   # локально: что архивируем
#   remote_site_dir=/webserver/madmentat.ru  # удалённо: куда разворачиваем
#   remote_backup_base=/webserver/.backup    # удалённо: где хранить бэкапы
#
# Веб-название:
#   server_name=madmentat.ru
#
# PHP:
#   php_version=8.3
#   # или:
#   # php_fpm_sock=/run/php/php8.3-fpm.sock
#
# База:
#   db_user=madmentat
#   db_pass=XXX
#   db_name=mad
#
# Прокси/переключение (nginx):
#   proxy_target=192.168.88.198
#   local_http_port=8081 - этот порт будет использован для конфигурации сервера на удаленном хосте
#   local_https_port=0
#
#   health_url= "http://127.0.0.1:80/" - адрес и порт для проверки статуса локального вебсервера
#   switch_to_local=true
#   ssl_cert=/path/to/cert.crt
#   ssl_key=/path/to/key.key
#
# Планировщик:
#   schedule_hhmm=04:00
#   health_interval_sec=60
#
# Примеры:
#   ./madbackuper --target-server=nginx --switch-to-local=true
#   ./madbackuper --skip-tar
#   ./madbackuper --skip-upload
#   ./madbackuper --daemon --at=03:00
# ------------------------------------------------------------
)";

void write_default_config(const std::string& path) {
    std::ofstream out(path);
    out << CFG_HEADER
        << "target_server="       << DEFAULT_TARGET             << "\n"
        << "remote_host="         << "192.168.88.202"           << "\n"
        << "ssh_port="            << DEFAULT_SSH_PORT           << "\n"
        << "remote_user="         << "madmentat"                << "\n"
        << "remote_pass="         << "XXX"                 << "\n"
        << "remote_root_pass="    << "XXX"                 << "\n"
        << "remote_sudo_pass="    << ""                         << "\n"
        << "local_site_dir="      << "/webserver/madmentat.ru"  << "\n"
        << "remote_site_dir="     << "/webserver/madmentat.ru"  << "\n"
        << "remote_backup_base="  << "/webserver/.backup"       << "\n"
        << "server_name="         << "madmentat.ru"             << "\n"
        << "php_version="         << DEFAULT_PHP_VERSION        << "\n"
        << "php_fpm_sock="        << ""                         << "\n"
        << "db_user="             << "madmentat"                << "\n"
        << "db_pass="             << "XXX"                 << "\n"
        << "db_name="             << "mad"                      << "\n"
        << "proxy_target="        << "192.168.88.198"           << "\n"
        << "local_http_port="     << DEFAULT_LOCAL_HTTP_PORT    << "\n"
        << "local_https_port="    << DEFAULT_LOCAL_HTTPS_PORT   << "\n"
        << "health_url="          << "http://127.0.0.1:80/"     << "\n"
        << "health_host_header="  << ""                         << "\n"
        << "switch_to_local="     << "true"                     << "\n"
        << "ssl_cert="            << ""                         << "\n"
        << "ssl_key="             << ""                         << "\n"
        << "health_interval_sec=" << DEFAULT_HEALTH_INTERVAL_SEC<< "\n"
        << "schedule_hhmm="       << "04:00"                    << "\n";
    out.close();
}

void load_kv_file(const std::string& path, Config& cfg) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0]=='#' || line[0]==';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto k = trim(line.substr(0, eq));
        auto v = trim(line.substr(eq+1));

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
    if (!notEmpty(c.proxy_target, "proxy_target")) return false;
    if (c.local_http_port <= 0 || c.local_http_port == 80 || c.local_http_port == 443) { err = "local_http_port >0 и не 80/443"; return false; }
    if (c.local_https_port > 0) {
        if (!notEmpty(c.ssl_cert, "ssl_cert")) return false;
        if (!notEmpty(c.ssl_key, "ssl_key")) return false;
        if (c.local_https_port == 80 || c.local_https_port == 443 || c.local_https_port == c.local_http_port) {
            err = "local_https_port >0, не 80/443 и ≠ local_http_port"; return false;
        }
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
