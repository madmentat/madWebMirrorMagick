// madbackuper.cpp
// ------------------------------------------------------------
// Бэкап и развёртывание на резервный хост (Ubuntu/Debian).
// Новое:
//   • --daemon + --at=HH:MM (по умолчанию 03:00)
//   • --skip-tar / --skip-sql / --skip-upload
//   • Проверка наличия nginx без зависимости от PATH
//   • Привилегированные действия через sudo (учёт отдельного remote_sudo_pass)
//   • Nginx: systemctl restart/reload, детальные проверки vhost (nginx -T)
//   • БД (MariaDB/MySQL): лог импорта и число таблиц после заливки
//   • Прогресс архивации/передачи, проверка места, bind-mount /webserver
//   • Автосоздание sudoers и нужных каталогов
//   • Автогенерация /root/setup_madmentat_nginx.sh и работа с /webserver/wachdog
//
// Сборка:
//   g++ -std=c++17 madbackuper.cpp -o madbackuper -lssh
//
// Примеры:
//   ./madbackuper --target-server=nginx --skip-upload
//   ./madbackuper --daemon --at=03:00 --target-server=nginx --php-version=8.3
// ------------------------------------------------------------
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <chrono>
#include <thread>
#include <string>
#include <type_traits>
#include <iomanip>
#include <memory>
#include <algorithm>
#include <csignal>
#include <atomic>

namespace fs = std::filesystem;
using clock_ = std::chrono::steady_clock;

static std::atomic<bool> g_stop{false};
static void on_sigint (int){ g_stop = true; }
static void on_sigterm(int){ g_stop = true; }

// --------------------------- Время/сон ---------------------------
static std::time_t next_local_time(int hh, int mm) {
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    lt.tm_hour = hh; lt.tm_min = mm; lt.tm_sec = 0;
    std::time_t t = std::mktime(&lt);
    if (t <= now) { lt.tm_mday += 1; t = std::mktime(&lt); }
    return t;
}
static void sleep_until_epoch(std::time_t target) {
    while (!g_stop) {
        std::time_t now = std::time(nullptr);
        if (now >= target) break;
        auto remain = target - now;
        std::this_thread::sleep_for(std::chrono::seconds(remain > 60 ? 60 : remain));
    }
}

// --------------------------- Константы ---------------------------
static const char* CFG_PATH_PRIMARY   = "/etc/madbackuper.conf";
static const char* CFG_PATH_FALLBACK  = "/root/madbackuper.conf";
static const char* DEFAULT_TARGET     = "nginx";      // или "apache2"
static const int   DEFAULT_SSH_PORT   = 22;
static const int   DEFAULT_LOCAL_HTTP_PORT  = 8080;
static const int   DEFAULT_LOCAL_HTTPS_PORT = 0;
static const bool  DEFAULT_SWITCH_TO_LOCAL  = true;
static const std::string DEFAULT_PHP_VERSION = "8.3";

// ===== base64 encoder =====
static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string base64_encode(const std::string &in) {
    std::string out; size_t i = 0; unsigned char a3[3]{}, a4[4]{};
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

// --------------------------- Конфиг ---------------------------
struct Config {
    // Основное
    std::string target_server = DEFAULT_TARGET; // nginx | apache2
    std::string remote_host   = "192.168.88.202";
    int         ssh_port      = DEFAULT_SSH_PORT;
    std::string remote_user   = "madmentat";
    std::string remote_pass   = "XXX";
    std::string remote_sudo_pass = "XXX"; // пусто => remote_pass
    std::string remote_root_pass = "XXX"; // на будущее

    // Пути
    std::string local_site_dir    = "/webserver/madmentat.ru";
    std::string remote_site_dir   = "/webserver/madmentat.ru";
    std::string remote_backup_base= "/webserver/.backup";

    // Веб-название
    std::string server_name   = "madmentat.ru";

    // PHP
    std::string php_version   = DEFAULT_PHP_VERSION;
    std::string php_fpm_sock  = ""; // если задано — перебивает php_version

    // База
    std::string db_user = "madmentat";
    std::string db_pass = "XXX";
    std::string db_name = "mad";

    // Прокси/переключение
    std::string proxy_target = "192.168.88.198";
    int         local_http_port  = DEFAULT_LOCAL_HTTP_PORT;
    int         local_https_port = DEFAULT_LOCAL_HTTPS_PORT;
    bool        switch_to_local  = DEFAULT_SWITCH_TO_LOCAL;

    // SSL для локального HTTPS
    std::string ssl_cert = "";
    std::string ssl_key  = "";

    // Флаги пропусков
    bool skip_tar    = false;
    bool skip_sql    = false;
    bool skip_upload = false;
};

static std::string trim(const std::string& s) {
    auto l = s.find_first_not_of(" \t\r\n");
    auto r = s.find_last_not_of(" \t\r\n");
    if (l == std::string::npos) return {};
    return s.substr(l, r - l + 1);
}
static std::string today() {
    char buf[16]; std::time_t t = std::time(nullptr); std::tm tm{};
    localtime_r(&t, &tm); std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
}
static std::string human_size(uint64_t bytes) {
    const char* u[] = {"B","KB","MB","GB","TB","PB"};
    double v = static_cast<double>(bytes); int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; ++i; }
    char buf[64]; std::snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
    return buf;
}
static uint64_t dir_size_bytes(const fs::path& root) {
    uint64_t total = 0; std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; ++it) {
        if (ec) { ec.clear(); continue; }
        std::error_code ec2;
        if (fs::is_regular_file(*it, ec2)) total += fs::file_size(*it, ec2);
    }
    return total;
}
static bool has_command(const char* name) {
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// --------------------------- Локальные запускашки ---------------------------
static int run_local(const std::string& cmd, bool echo = true) {
    if (echo) std::cout << "➜ " << cmd << "\n";
    int rc = std::system(cmd.c_str());
    if (rc != 0) std::cerr << "❌ Команда вернула код " << rc << "\n";
    return rc;
}
static int run_with_spinner(const std::string& cmd, const std::string& label) {
    pid_t pid = fork();
    if (pid < 0) { std::cerr << "❌ fork() для: " << cmd << "\n"; return -1; }
    if (pid == 0) { execl("/bin/sh", "sh", "-lc", cmd.c_str(), (char*)nullptr); _exit(127); }
    auto start = clock_::now();
    const char* frames[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
    size_t idx = 0;
    int status = 0;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(clock_::now() - start).count();
            std::cout << "\r" << label << " " << frames[idx % 10] << "  " << elapsed << "s" << std::flush;
            idx++;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        } else break;
    }
    std::cout << "\r" << label << " ✓                                     " << std::endl;
    if (!WIFEXITED(status)) { std::cerr << "❌ Процесс завершён ненормально\n"; return -1; }
    int rc = WEXITSTATUS(status);
    if (rc != 0) std::cerr << "❌ Команда вернула код " << rc << "\n";
    return rc;
}
static std::string peer_ip(ssh_session s) {
    int sock = ssh_get_fd(s);
    sockaddr_storage addr{}; socklen_t len = sizeof(addr);
    if (getpeername(sock, (sockaddr*)&addr, &len) != 0) return "unknown";
    char ip[INET6_ADDRSTRLEN]{};
    if (addr.ss_family == AF_INET) {
        auto* a = (sockaddr_in*)&addr; inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    } else if (addr.ss_family == AF_INET6) {
        auto* a = (sockaddr_in6*)&addr; inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
    } else return "unknown";
    return ip;
}

// --------------------------- SSH helpers ---------------------------
static int ssh_exec(ssh_session session, const std::string& cmd, bool print_out = true) {
    ssh_channel ch = ssh_channel_new(session);
    if (!ch) { std::cerr << "❌ ssh_channel_new: " << ssh_get_error(session) << "\n"; return -1; }
    if (ssh_channel_open_session(ch) != SSH_OK) {
        std::cerr << "❌ ssh_channel_open_session: " << ssh_get_error(session) << "\n"; ssh_channel_free(ch); return -1;
    }
    if (ssh_channel_request_exec(ch, cmd.c_str()) != SSH_OK) {
        std::cerr << "❌ ssh_channel_request_exec: " << ssh_get_error(session) << "\n"; ssh_channel_close(ch); ssh_channel_free(ch); return -1;
    }
    if (print_out) {
        char buf[4096]; int n;
        while ((n = ssh_channel_read(ch, buf, sizeof(buf), 0)) > 0) std::cout.write(buf, n);
        while ((n = ssh_channel_read(ch, buf, sizeof(buf), 1)) > 0) std::cerr.write(buf, n);
    } else {
        char buf[4096]; int n;
        while ((n = ssh_channel_read(ch, buf, sizeof(buf), 0)) > 0) {}
        while ((n = ssh_channel_read(ch, buf, sizeof(buf), 1)) > 0) {}
    }
    ssh_channel_send_eof(ch);
    int exit_status = ssh_channel_get_exit_status(ch);
    ssh_channel_close(ch); ssh_channel_free(ch);
    return exit_status;
}
static int ssh_exec_capture(ssh_session session, const std::string& cmd, std::string& out, std::string* err=nullptr) {
    out.clear(); if (err) err->clear();
    ssh_channel ch = ssh_channel_new(session);
    if (!ch) return -1;
    if (ssh_channel_open_session(ch) != SSH_OK) { ssh_channel_free(ch); return -1; }
    if (ssh_channel_request_exec(ch, cmd.c_str()) != SSH_OK) { ssh_channel_close(ch); ssh_channel_free(ch); return -1; }
    char buf[4096]; int n;
    while ((n = ssh_channel_read(ch, buf, sizeof(buf), 0)) > 0) out.append(buf, n);
    if (err) while ((n = ssh_channel_read(ch, buf, sizeof(buf), 1)) > 0) err->append(buf, n);
    ssh_channel_send_eof(ch);
    int exit_status = ssh_channel_get_exit_status(ch);
    ssh_channel_close(ch); ssh_channel_free(ch);
    return exit_status;
}

// --------------------------- Конфиг I/O ---------------------------
static const char* CFG_HEADER =
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
#   local_http_port=8080
#   local_https_port=0
#   switch_to_local=true
#   ssl_cert=/path/to/cert.crt
#   ssl_key=/path/to/key.key
#
# Примеры:
#   ./madbackuper --target-server=nginx --switch-to-local=true
#   ./madbackuper --skip-tar
#   ./madbackuper --skip-upload
#   ./madbackuper --daemon --at=03:00
# ------------------------------------------------------------
)";
static void write_default_config(const std::string& path) {
    std::ofstream out(path);
    out << CFG_HEADER
        << "target_server="      << DEFAULT_TARGET            << "\n"
        << "remote_host="        << "192.168.88.202"          << "\n"
        << "ssh_port="           << DEFAULT_SSH_PORT          << "\n"
        << "remote_user="        << "madmentat"               << "\n"
        << "remote_pass="        << "XXX"                << "\n"
        << "remote_root_pass="   << "XXX"                << "\n"
        << "local_site_dir="     << "/webserver/madmentat.ru" << "\n"
        << "remote_site_dir="    << "/webserver/madmentat.ru" << "\n"
        << "remote_backup_base=" << "/webserver/.backup"      << "\n"
        << "server_name="        << "madmentat.ru"            << "\n"
        << "php_version="        << DEFAULT_PHP_VERSION       << "\n"
        << "php_fpm_sock="       << ""                        << "\n"
        << "db_user="            << "madmentat"               << "\n"
        << "db_pass="            << "XXX"                << "\n"
        << "db_name="            << "mad"                     << "\n"
        << "proxy_target="       << "192.168.88.198"          << "\n"
        << "local_http_port="    << DEFAULT_LOCAL_HTTP_PORT   << "\n"
        << "local_https_port="   << DEFAULT_LOCAL_HTTPS_PORT  << "\n"
        << "switch_to_local="    << "true"                    << "\n"
        << "ssl_cert="           << ""                        << "\n"
        << "ssl_key="            << ""                        << "\n";
    out.close();
}
static void load_kv_file(const std::string& path, Config& cfg) {
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
        if      (k=="target_server")                cfg.target_server = v;
        else if (k=="remote_host")                  cfg.remote_host = v;
        else if (k=="ssh_port")                     cfg.ssh_port = std::stoi(v);
        else if (k=="remote_user")                  cfg.remote_user = v;
        else if (k=="remote_pass")                  cfg.remote_pass = v;
        else if (k=="remote_root_pass")             cfg.remote_root_pass = v;
        else if (k=="remote_sudo_pass")             cfg.remote_sudo_pass = v;
        else if (k=="local_site_dir")               cfg.local_site_dir = v;
        else if (k=="remote_site_dir")              cfg.remote_site_dir = v;
        else if (k=="remote_backup_base")           cfg.remote_backup_base = v;
        else if (k=="server_name")                  cfg.server_name = v;
        else if (k=="php_version")                  cfg.php_version = v;
        else if (k=="php_fpm_sock")                 cfg.php_fpm_sock = v;
        else if (k=="db_user")                      cfg.db_user = v;
        else if (k=="db_pass")                      cfg.db_pass = v;
        else if (k=="db_name")                      cfg.db_name = v;
        else if (k=="proxy_target")                 cfg.proxy_target = v;
        else if (k=="local_http_port")              cfg.local_http_port = std::stoi(v);
        else if (k=="local_https_port")             cfg.local_https_port = std::stoi(v);
        else if (k=="switch_to_local")              cfg.switch_to_local = (v=="true"||v=="1"||v=="yes");
        else if (k=="ssl_cert")                     cfg.ssl_cert = v;
        else if (k=="ssl_key")                      cfg.ssl_key = v;
    }
}
static bool parse_hhmm(const std::string& s, int& hh, int& mm) {
    if (s.size()!=5 || s[2]!=':') return false;
    try { hh=std::stoi(s.substr(0,2)); mm=std::stoi(s.substr(3,2)); } catch(...) { return false; }
    return (0<=hh && hh<24) && (0<=mm && mm<60);
}
static void apply_cli_kv(int argc, char** argv, Config& cfg,
                         bool& daemon_mode, int& alarm_h, int& alarm_m) {
    auto eat = [&](const std::string& arg, const char* key, auto& dst) {
        std::string p = std::string("--") + key + "=";
        if (arg.rfind(p, 0) == 0) {
            std::string val = arg.substr(p.size());
            if constexpr(std::is_same_v<decltype(dst), int&>) dst = std::stoi(val);
            else if constexpr(std::is_same_v<decltype(dst), bool&>) dst = (val=="true"||val=="1"||val=="yes");
            else dst = val;
            return true;
        }
        return false;
    };
    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        // key=value
        if (eat(a, "target-server",           cfg.target_server)) continue;
        if (eat(a, "remote-host",             cfg.remote_host))   continue;
        if (eat(a, "ssh-port",                cfg.ssh_port))      continue;
        if (eat(a, "remote-user",             cfg.remote_user))   continue;
        if (eat(a, "remote-pass",             cfg.remote_pass))   continue;
        if (eat(a, "remote-root-pass",        cfg.remote_root_pass)) continue;
        if (eat(a, "remote-sudo-pass",        cfg.remote_sudo_pass)) continue;
        if (eat(a, "local-site-dir",          cfg.local_site_dir)) continue;
        if (eat(a, "remote-site-dir",         cfg.remote_site_dir)) continue;
        if (eat(a, "remote-backup-base",      cfg.remote_backup_base)) continue;
        if (eat(a, "server-name",             cfg.server_name))   continue;
        if (eat(a, "php-version",             cfg.php_version))   continue;
        if (eat(a, "php-fpm-sock",            cfg.php_fpm_sock))  continue;
        if (eat(a, "db-user",                 cfg.db_user))       continue;
        if (eat(a, "db-pass",                 cfg.db_pass))       continue;
        if (eat(a, "db-name",                 cfg.db_name))       continue;
        if (eat(a, "proxy-target",            cfg.proxy_target))  continue;
        if (eat(a, "local-http-port",         cfg.local_http_port)) continue;
        if (eat(a, "local-https-port",        cfg.local_https_port)) continue;
        if (eat(a, "switch-to-local",         cfg.switch_to_local)) continue;
        if (eat(a, "ssl-cert",                cfg.ssl_cert))      continue;
        if (eat(a, "ssl-key",                 cfg.ssl_key))       continue;

        // флаги
        if (a=="--skip-tar")      { cfg.skip_tar = true; continue; }
        if (a=="--skip-sql")      { cfg.skip_sql = true; continue; }
        if (a=="--skip-upload")   { cfg.skip_upload = true; continue; }
        if (a=="--daemon")        { daemon_mode = true; continue; }
        if (a.rfind("--at=",0)==0){
            std::string v = a.substr(5);
            if (!parse_hhmm(v, alarm_h, alarm_m)) {
                std::cerr << "❌ Неверный формат --at=HH:MM\n"; std::exit(1);
            }
            continue;
        }
    }
}

// --------------------------- Проверка параметров ---------------------------
static bool validate(const Config& c, std::string& err) {
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

// --------------------------- SFTP helpers ---------------------------
static const char* sftp_errname(int code) {
    switch (code) {
        case SSH_FX_OK: return "OK";
        case SSH_FX_EOF: return "EOF";
        case SSH_FX_NO_SUCH_FILE: return "NO_SUCH_FILE";
        case SSH_FX_PERMISSION_DENIED: return "PERMISSION_DENIED";
        case SSH_FX_FAILURE: return "FAILURE";
        case SSH_FX_BAD_MESSAGE: return "BAD_MESSAGE";
        case SSH_FX_NO_CONNECTION: return "NO_CONNECTION";
        case SSH_FX_CONNECTION_LOST: return "CONNECTION_LOST";
        case SSH_FX_OP_UNSUPPORTED: return "OP_UNSUPPORTED";
        default: return "UNKNOWN";
    }
}
static int sftp_mkdirs(ssh_session session, sftp_session sftp, const std::string& path, mode_t mode = 0755) {
    if (path.empty()) return SSH_OK;
    std::string cur;
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i==path.size() || path[i]=='/') {
            cur = path.substr(0, i);
            if (cur.empty()) continue;
            int rc = sftp_mkdir(sftp, cur.c_str(), mode);
            if (rc != SSH_OK) {
                int err = sftp_get_error(sftp);
                if (err != SSH_FX_FILE_ALREADY_EXISTS) {
                    std::cerr << "❌ sftp_mkdir(" << cur << "): err " << err
                              << " (" << sftp_errname(err) << ") - " << ssh_get_error(session) << "\n";
                    return rc;
                }
            }
        }
    }
    return SSH_OK;
}
static void print_progress_line(const std::string& label, uint64_t sent, uint64_t total, double mbps) {
    double ratio = total ? (double)sent / (double)total : 0.0;
    int width = 28; int fill = (int)(ratio * width);
    uint64_t remain = total > sent ? (total - sent) : 0;
    double eta = (mbps>0) ? (remain/1024.0/1024.0)/mbps : 0.0;
    std::cout << "\r" << label << " [";
    for (int i=0;i<width;i++) std::cout << (i<fill ? "█" : " ");
    std::cout << "] " << std::fixed << std::setprecision(1)
              << (ratio*100.0) << "%  " << std::setprecision(2) << mbps << " MB/s  "
              << human_size(sent) << "/" << human_size(total)
              << "  ETA " << std::setprecision(0) << eta << "s" << std::flush;
}
static int sftp_upload_file_progress(ssh_session session, sftp_session sftp,
                                     const std::string& local, const std::string& remote,
                                     const std::string& label, int* out_err=nullptr, mode_t mode = 0644) {
    if (out_err) *out_err = SSH_FX_OK;
    std::ifstream in(local, std::ios::binary);
    if (!in) { std::cerr << "❌ Не открыть локальный файл: " << local << "\n"; return -1; }
    uint64_t total = 0; try { total = fs::file_size(local); } catch (...) { total = 0; }
    sftp_file f = sftp_open(sftp, remote.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (!f) {
        int err = sftp_get_error(sftp);
        if (out_err) *out_err = err;
        std::cerr << "❌ sftp_open(" << remote << "): " << ssh_get_error(session)
                  << " | SFTP err=" << err << " (" << sftp_errname(err) << ")\n";
        return -1;
    }
    const size_t BUFSZ = 64 * 1024;
    std::unique_ptr<char[]> buf(new char[BUFSZ]);
    uint64_t sent = 0; auto t0 = clock_::now(); auto last = t0;
    while (in) {
        in.read(buf.get(), BUFSZ);
        std::streamsize left = in.gcount();
        char* p = buf.get();
        while (left > 0) {
            ssize_t wr = sftp_write(f, p, left);
            if (wr < 0) {
                int err = sftp_get_error(sftp);
                if (out_err) *out_err = err;
                std::cerr << "\n❌ sftp_write(" << remote << "): " << ssh_get_error(session)
                          << " | SFTP err=" << err << " (" << sftp_errname(err) << ")\n";
                sftp_close(f);
                return -1;
            }
            p += wr; left -= wr; sent += wr;
            auto now = clock_::now();
            auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
            if (dt_ms >= 250 || sent == total) {
                double secs = std::chrono::duration<double>(now - t0).count();
                double mbps = secs>0 ? (sent/1024.0/1024.0)/secs : 0.0;
                print_progress_line(label, sent, total, mbps);
                last = now;
            }
        }
    }
    sftp_close(f);
    std::cout << "\r" << label << " [████████████████████████████] 100.0%  ✓  "
              << human_size(sent) << "                        " << std::endl;
    return 0;
}
static int ssh_stream_upload(ssh_session session,
                             const std::string& local, const std::string& remote,
                             const std::string& label) {
    std::string remote_part = remote + ".part";
    ssh_channel ch = ssh_channel_new(session);
    if (!ch) { std::cerr << "❌ ssh_channel_new\n"; return -1; }
    if (ssh_channel_open_session(ch) != SSH_OK) {
        std::cerr << "❌ ssh_channel_open_session: " << ssh_get_error(session) << "\n";
        ssh_channel_free(ch); return -1;
    }
    std::string cmd = "sh -lc 'umask 022; cat > " + remote_part + "'";
    if (ssh_channel_request_exec(ch, cmd.c_str()) != SSH_OK) {
        std::cerr << "❌ ssh_channel_request_exec (cat): " << ssh_get_error(session) << "\n";
        ssh_channel_close(ch); ssh_channel_free(ch); return -1;
    }
    std::ifstream in(local, std::ios::binary);
    if (!in) { std::cerr << "❌ Не открыть локальный файл: " << local << "\n";
        ssh_channel_close(ch); ssh_channel_free(ch); return -1; }
    uint64_t total = 0; try { total = fs::file_size(local); } catch (...) { total = 0; }
    const size_t BUFSZ = 128 * 1024;
    std::unique_ptr<char[]> buf(new char[BUFSZ]);
    uint64_t sent = 0; auto t0 = clock_::now(); auto last = t0;
    while (in) {
        in.read(buf.get(), BUFSZ);
        std::streamsize n = in.gcount();
        if (n <= 0) break;
        const char* p = buf.get();
        while (n > 0) {
            int wr = ssh_channel_write(ch, p, (uint32_t)n);
            if (wr == SSH_ERROR) {
                std::cerr << "\n❌ ssh_channel_write: " << ssh_get_error(session) << "\n";
                ssh_channel_send_eof(ch);
                ssh_channel_close(ch);
                ssh_channel_free(ch);
                return -1;
            }
            p += wr; n -= wr; sent += wr;
            auto now = clock_::now();
            auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
            if (dt_ms >= 250 || sent == total) {
                double secs = std::chrono::duration<double>(now - t0).count();
                double mbps = secs>0 ? (sent/1024.0/1024.0)/secs : 0.0;
                print_progress_line(label, sent, total, mbps);
                last = now;
            }
        }
    }
    ssh_channel_send_eof(ch);
    int exit_status = ssh_channel_get_exit_status(ch);
    ssh_channel_close(ch); ssh_channel_free(ch);
    std::cout << "\r" << label << " [████████████████████████████] 100.0%  ✓  "
              << human_size(sent) << "                        " << std::endl;
    if (exit_status != 0) {
        std::cerr << "❌ Удалённая команда cat завершилась с кодом " << exit_status << "\n";
        return -1;
    }
    // атомарно переименовать
    {
        std::ostringstream mv;
        mv << "sh -lc 'mv -f " << remote_part << " " << remote << "'";
        if (ssh_exec(session, mv.str(), true) != 0) {
            std::cerr << "❌ Не удалось переименовать " << remote_part << " -> " << remote << "\n";
            return -1;
        }
    }
    return 0;
}

// --------------------------- sudo helpers ---------------------------
static std::string sh_escape_single(const std::string& s) {
    std::string out; out.reserve(s.size()+8);
    for (char c: s) { if (c=='\'') out += "'\\''"; else out += c; }
    return out;
}
static std::string sudo_prefix(ssh_session session, const Config& C) {
    if (C.remote_user == "root") return "";
    // 1) пробуем без пароля
    {
        ssh_channel ch = ssh_channel_new(session);
        if (ch && ssh_channel_open_session(ch) == SSH_OK &&
            ssh_channel_request_exec(ch, "sudo -n true") == SSH_OK) {
            ssh_channel_send_eof(ch);
            int rc = ssh_channel_get_exit_status(ch);
            ssh_channel_close(ch); ssh_channel_free(ch);
            if (rc == 0) return "sudo -n";
        } else if (ch) { ssh_channel_close(ch); ssh_channel_free(ch); }
    }
    // 2) через stdin
    std::string sudopw = C.remote_sudo_pass.empty() ? C.remote_pass : C.remote_sudo_pass;
    return "echo '" + sh_escape_single(sudopw) + "' | sudo -S -p ''";
}

// --------------------------- Проверка места / bind-mount ---------------------------
static uint64_t remote_bytes_avail(ssh_session session, const std::string& path) {
    std::string out, err;
    int rc = ssh_exec_capture(session,
        "df -B1 --output=avail '" + path + "' 2>/dev/null | tail -n1 | tr -d '[:space:]'", out, &err);
    if (rc != 0 || out.empty()) return 0;
    try { return std::stoull(trim(out)); } catch (...) { return 0; }
}
static int ensure_space_and_bind_mount(ssh_session session, const Config& C, uint64_t need_bytes) {
    std::cout << "💽 Проверка свободного места на удалёнке...\n";
    uint64_t avail = remote_bytes_avail(session, "/webserver");
    if (avail == 0) {
        std::string SUDO = sudo_prefix(session, C);
        ssh_exec(session, (SUDO.empty()? "" : (SUDO + " ")) + "mkdir -p /webserver", false);
        avail = remote_bytes_avail(session, "/webserver");
    }
    uint64_t headroom = std::max<uint64_t>(need_bytes / 10, 512ULL*1024*1024); // 10% или 512MB
    uint64_t need_total = need_bytes + headroom;
    std::cout << "   Нужно ~" << human_size(need_total) << ", доступно ~" << human_size(avail) << "\n";
    if (avail >= need_total) { std::cout << "✅ Места достаточно на текущем разделе.\n"; return 0; }
    std::cout << "⚠️  Места недостаточно — bind-mount $HOME/webserver -> /webserver...\n";
    std::string SUDO = sudo_prefix(session, C);
    std::string home;
    {
        std::string out;
        if (ssh_exec_capture(session, "printf %s \"$HOME\"", out, nullptr) == 0 && !trim(out).empty()) home = trim(out);
        else {
            std::string out2;
            if (ssh_exec_capture(session, "getent passwd \"$USER\" | cut -d: -f6", out2, nullptr) == 0 && !trim(out2).empty())
                home = trim(out2);
        }
        if (home.empty()) home = "/home/" + C.remote_user;
    }
    std::string prep =
        (SUDO.empty()? "" : (SUDO + " ")) + "mkdir -p '" + home + "/webserver' /webserver && "
        + (C.remote_user=="root" ? "true" :
           (SUDO.empty()? "" : (SUDO + " ")) + "chown -R " + C.remote_user + ":" + C.remote_user + " '" + home + "/webserver'") + " && "
        + (SUDO.empty()? "" : (SUDO + " ")) + "mountpoint -q /webserver || "
        + (SUDO.empty()? "" : (SUDO + " ")) + "mount --bind '" + home + "/webserver' /webserver && "
        + "echo '" + home + "/webserver /webserver none bind 0 0' | "
        + (SUDO.empty()? "" : (SUDO + " ")) + "tee -a /etc/fstab >/dev/null";
    if (ssh_exec(session, prep, true) != 0) {
        std::cerr << "❌ Не удалось выполнить bind-mount /webserver\n";
        return 1;
    }
    uint64_t avail2 = remote_bytes_avail(session, "/webserver");
    std::cout << "   После монтирования доступно ~" << human_size(avail2) << "\n";
    if (avail2 < need_total) { std::cerr << "❌ Даже после bind-mount места недостаточно.\n"; return 1; }
    std::cout << "✅ Места достаточно после bind-mount.\n";
    return 0;
}

// --------------------------- Bootstrap удалёнки ---------------------------
static int bootstrap_remote(ssh_session session, const Config& C) {
    std::cout << "🛠️  Подготовка удалённого хоста...\n";
    std::string SUDO = sudo_prefix(session, C);
    // sudoers
    if (C.remote_user != "root") {
        const std::string sudoers = "/etc/sudoers.d/madbackuper";
        std::ostringstream content;
        content << "Cmnd_Alias MADBACKUP_CMDS = "
                << "/usr/sbin/nginx, /usr/sbin/nginx -t, "
                << "/bin/systemctl reload nginx, /bin/systemctl restart nginx, "
                << "/usr/sbin/apache2ctl, /bin/systemctl reload apache2, /bin/systemctl restart apache2, "
                << "/usr/bin/tee, /bin/mkdir, /bin/chown, /bin/chmod, /bin/ln, /bin/cp, /bin/mv, /bin/tar, /usr/bin/find, "
                << "/bin/mount, /bin/umount, /bin/mountpoint, /usr/bin/grep, /usr/bin/cut, /usr/bin/getent, "
                << "/usr/bin/mysql, /usr/bin/mysqldump\n"
                << C.remote_user << " ALL=(root) NOPASSWD: MADBACKUP_CMDS\n";
        std::ostringstream ensure;
        ensure
          << "if [ -f '" << sudoers << "' ]; then "
          << "  if ! grep -q 'MADBACKUP_CMDS' '" << sudoers << "'; then NEED=1; else "
          << "    for k in nginx mount mountpoint systemctl tar tee mkdir chown chmod ln mv cp find mysql mysqldump; do "
          << "      grep -q \"$k\" '" << sudoers << "' || { NEED=1; break; }; "
          << "    done; "
          << "  fi; "
          << "else NEED=1; fi; "
          << "if [ \"$NEED\" = 1 ]; then "
          << "  printf '%s' '" << sh_escape_single(content.str()) << "' | " << SUDO << " tee '" << sudoers << "' >/dev/null && "
          << SUDO << " chmod 440 '" << sudoers << "' && " << SUDO << " visudo -cf '" << sudoers << "'; "
          << "else echo '→ sudoers уже корректный: " << sudoers << "'; fi";
        ssh_exec(session, ensure.str(), true);
    }
    // каталоги и права
    {
        std::ostringstream cmd;
        cmd << (SUDO.empty()? "" : (SUDO + " "))
            << "mkdir -p '" << C.remote_backup_base << "' '" << C.remote_site_dir << "' && ";
        if (C.remote_user != "root") {
            std::string chown_cmd = (SUDO.empty()? "" : (SUDO + " "));
            chown_cmd += "chown -R " + C.remote_user + ":" + C.remote_user + " '" + C.remote_backup_base + "'";
            cmd << chown_cmd;
        } else cmd << "true";
        if (ssh_exec(session, cmd.str(), true) != 0) {
            std::cerr << "❌ Не удалось подготовить каталоги на удалёнке\n";
            return 1;
        }
    }
    std::cout << "✅ Удалённый хост подготовлен\n";
    return 0;
}

// --------------------------- Команда развёртывания (NGINX) ---------------------------
static std::string build_nginx_deploy_cmd(const Config& C,
                                          const std::string& remote_tar,
                                          const std::string& remote_sql,
                                          const std::string& remote_day)
{
    auto dq = [](const std::string& s) {
        std::string r; r.reserve(s.size()*2);
        for (char c : s) { if (c == '\\' || c == '"') r.push_back('\\'); r.push_back(c); }
        return r;
    };
    const std::string php_sock_cfg = C.php_fpm_sock.empty()
        ? ("/run/php/php" + C.php_version + "-fpm.sock")
        : C.php_fpm_sock;

    std::ostringstream root;
    root
    << "set -e; umask 022;\n"
    << "echo \"🔑 Переключаюсь на пользователя root\" 1>&2;\n"
    << "echo \"🔧 Проверяю окружение (nginx/mysql/tar)\" 1>&2;\n"
    << "command -v tar   >/dev/null || { echo \"❌ tar не установлен\" 1>&2; exit 1; }\n"
    << "command -v mysql >/dev/null || { echo \"❌ mysql клиент не установлен\" 1>&2; exit 1; }\n"
    << "NGINX_OK=0; if [ -x /usr/sbin/nginx ]; then NGINX_OK=1; fi;\n"
    << "command -v nginx >/dev/null 2>&1 && NGINX_OK=1;\n"
    << "systemctl -q is-active nginx >/dev/null 2>&1 && NGINX_OK=1;\n"
    << "[ -d /etc/nginx ] && NGINX_OK=1;\n"
    << "if [ \"$NGINX_OK\" -ne 1 ]; then echo \"❌ nginx не установлен\" 1>&2; exit 1; fi;\n"

    << "echo \"📦 Подготавливаю директории сайта\" 1>&2;\n"
    << "mkdir -p \"" << dq(C.remote_site_dir) << "\"\n"
    << "rm -rf \"" << dq(C.remote_site_dir) << ".old\"\n"
    << "mv \"" << dq(C.remote_site_dir) << "\" \"" << dq(C.remote_site_dir) << ".old\" 2>/dev/null || true\n"
    << "mkdir -p \"" << dq(C.remote_site_dir) << "\"\n"

    << "echo \"📤 Распаковка архива сайта\" 1>&2;\n"
    << "if command -v pv >/dev/null 2>&1; then "
         "pv -f -p -t -e -r -b \"" << dq(remote_tar) << "\" | tar -xzf - -C \"" << dq(C.remote_site_dir) << "\"; "
       "else "
         "tar -xzf \"" << dq(remote_tar) << "\" -C \"" << dq(C.remote_site_dir) << "\" --checkpoint=500 --checkpoint-action=echo=. ; echo; "
       "fi\n"

    << "REF=$(mktemp); touch \"$REF\"; "
       "find \"" << dq(C.remote_site_dir) << "\" \\( -type f -o -type d \\) -newer \"$REF\" -print0 | xargs -0 -r touch -r \"$REF\"; "
       "rm -f \"$REF\"\n"

    << "echo \"🧰 Выставляю права\" 1>&2;\n"
    << "chown -R www-data:www-data \"" << dq(C.remote_site_dir) << "\" || true\n"
    << "find \"" << dq(C.remote_site_dir) << "\" -type d -exec chmod 755 {} \\; || true\n"
    << "find \"" << dq(C.remote_site_dir) << "\" -type f -exec chmod 644 {} \\; || true\n"

    // БД
    << "echo \"🗄️  Подготавливаю БД и пользователя (MariaDB)\" 1>&2;\n"
    << "/usr/bin/mysql -uroot <<\\SQL\n"
       "CREATE DATABASE IF NOT EXISTS `" << C.db_name << "` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;\n"
       "CREATE USER IF NOT EXISTS '" << C.db_user << "'@'localhost' IDENTIFIED BY '" << C.db_pass << "';\n"
       "CREATE USER IF NOT EXISTS '" << C.db_user << "'@'127.0.0.1' IDENTIFIED BY '" << C.db_pass << "';\n"
       "GRANT ALL ON `" << C.db_name << "`.* TO '" << C.db_user << "'@'localhost';\n"
       "GRANT ALL ON `" << C.db_name << "`.* TO '" << C.db_user << "'@'127.0.0.1';\n"
       "FLUSH PRIVILEGES;\n"
    "SQL\n"
    << "echo \"📦 Бэкап прежней БД (мягко)\" 1>&2;\n"
    << "/usr/bin/mysqldump \"" << dq(C.db_name) << "\" > \"" << dq(remote_day) << "/db_old.sql\" 2>/dev/null || true\n"
    << "DB_IMPORTED=0; TABLES_AFTER=0;\n"
    << "if [ -s \"" << dq(remote_sql) << "\" ]; then "
         "echo \"⬇️  Импортирую дамп\" 1>&2; "
         "/usr/bin/mysql \"" << dq(C.db_name) << "\" < \"" << dq(remote_sql) << "\" && DB_IMPORTED=1; "
       "else "
         "echo \"⚠️ Дамп не найден или пуст — пропуск\" 1>&2; "
       "fi\n"
    << "TABLES_AFTER=$(/usr/bin/mysql -NBe \"SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='"
      << dq(C.db_name) << "'\" 2>/dev/null || echo 0)\n"
    << "echo \"   📊 Таблиц в БД после импорта: $TABLES_AFTER\" 1>&2;\n"

    // setup_madmentat_nginx.sh + wachdog
    << "echo \"🧩 Проверяю /root/setup_madmentat_nginx.sh\" 1>&2;\n"
    << "if [ ! -x /root/setup_madmentat_nginx.sh ]; then\n"
    << "  echo \"✍️  Пишу /root/setup_madmentat_nginx.sh\" 1>&2;\n"
    << "  umask 077; cat > /root/setup_madmentat_nginx.sh <<'EOS'\n"
    << "#!/usr/bin/env bash\n"
       "set -euo pipefail\n"
       "ts(){ date +\"[%H:%M:%S]\"; }\n"
       "log(){ echo \"$(ts) $*\"; }\n"
       "DOMAIN=\"" << dq(C.server_name) << "\"; WWW=\"www.${DOMAIN}\"\n"
       "WEBROOT=\"" << dq(C.remote_site_dir) << "\"\n"
       "FRONT_AVAIL=\"/etc/nginx/sites-available/${DOMAIN}.conf\"\n"
       "FRONT_ENABLED=\"/etc/nginx/sites-enabled/${DOMAIN}.conf\"\n"
       "LOCAL_AVAIL=\"/etc/nginx/sites-available/${DOMAIN}.local.conf\"\n"
       "LOCAL_ENABLED=\"/etc/nginx/sites-enabled/${DOMAIN}.local.conf\"\n"
       "BACKEND_CONF=\"/etc/nginx/conf.d/${DOMAIN}.backend.conf\"\n"
       "BACKEND_LOCAL=\"http://127.0.0.1:8081\"\n"
       "BACKEND_REMOTE=\"http://192.168.88.198\"\n"
       "pick_cert_dir(){ for d in \"/etc/letsencrypt/live/${DOMAIN}-0002\" \"/etc/letsencrypt/live/${DOMAIN}\"; do\n"
       "  [ -s \"$d/fullchain.pem\" ] && [ -s \"$d/privkey.pem\" ] && { echo \"$d\"; return 0; }\n"
       "done; return 1; }\n"
       "detect_php_sock(){ for s in /run/php/php*-fpm.sock; do [ -S \"$s\" ] && { echo \"$s\"; return 0; }; done; echo \"/run/php/php-fpm.sock\"; }\n"
       "CERT_DIR=\"$(pick_cert_dir || true)\"; [ -z \"${CERT_DIR:-}\" ] && CERT_DIR=\"/etc/letsencrypt/live/${DOMAIN}-0002\"\n"
       "CERT_FULL=\"${CERT_DIR}/fullchain.pem\"; CERT_KEY=\"${CERT_DIR}/privkey.pem\"\n"
       "PHP_SOCK=\"$(detect_php_sock)\"\n"
       "MODE=\"${1:-status}\"; OVERRIDE_REMOTE=\"${2:-}\"\n"
       "case \"$MODE\" in\n"
       "  local)  TARGET=\"$BACKEND_LOCAL\" ;;\n"
       "  remote) TARGET=\"${OVERRIDE_REMOTE:-$BACKEND_REMOTE}\" ;;\n"
       "  status) TARGET=\"\" ;;\n"
       "  *) echo \"Usage: $0 {local|remote|status} [remote_url]\"; exit 1 ;;\n"
       "esac\n"
       "mkdir -p /etc/nginx/disabled\n"
       "for f in \\\n"
       "  \"/etc/nginx/sites-enabled/${DOMAIN}.local.conf\" \\\n"
       "  \"/etc/nginx/sites-available/${DOMAIN}.local.conf\" \\\n"
       "  \"/etc/nginx/sites-enabled/setup_${DOMAIN}_nginx.sh\" \\\n"
       "  \"/etc/nginx/sites-available/setup_${DOMAIN}_nginx.sh\" \\\n"
       "  ; do\n"
       "  [ -e \"$f\" ] || continue\n"
       "  log \"Карантин: $f\"; mv -f \"$f\" \"/etc/nginx/disabled/$(basename \"$f\").$(date +%Y%m%d-%H%M%S)\"\n"
       "done\n"
       "if [ -n \"$TARGET\" ]; then\n"
       "  log \"Пишу ${BACKEND_CONF} -> ${TARGET}\"; echo \"set \\$mad_backend ${TARGET};\" > \"${BACKEND_CONF}\"\n"
       "else\n"
       "  log \"Статус: BACKEND не меняю (${BACKEND_CONF})\"\n"
       "fi\n"
       "log \"Готовлю локальный backend ${LOCAL_AVAIL} (127.0.0.1:8081)\"\n"
       "cat > \"${LOCAL_AVAIL}\" <<EOF\n"
       "server {\n"
       "    listen 127.0.0.1:8081;\n"
       "    server_name _;\n"
       "    root ${WEBROOT};\n"
       "    index index.php index.html;\n"
       "    access_log /var/log/nginx/${DOMAIN}.local.access.log;\n"
       "    error_log  /var/log/nginx/${DOMAIN}.local.error.log;\n"
       "    location / { try_files \\$uri \\$uri/ /index.php?\\$args; }\n"
       "    location ~ \\.php$ { include snippets/fastcgi-php.conf; fastcgi_pass unix:${PHP_SOCK}; }\n"
       "    client_max_body_size 1024m;\n"
       "}\n"
       "EOF\n"
       "ln -sf \"${LOCAL_AVAIL}\" \"${LOCAL_ENABLED}\"\n"
       "log \"Пишу фронт ${FRONT_AVAIL}\"\n"
       "cat > \"${FRONT_AVAIL}\" <<EOF\n"
       "server {\n"
       "    listen 80;\n"
       "    listen [::]:80;\n"
       "    server_name ${DOMAIN} ${WWW};\n"
       "    client_max_body_size 1024m;\n"
       "    return 301 https://\\$host\\$request_uri;\n"
       "}\n"
       "server {\n"
       "    listen 443 ssl http2;\n"
       "    listen [::]:443 ssl http2;\n"
       "    server_name ${DOMAIN} ${WWW};\n"
       "    ssl_certificate     ${CERT_FULL};\n"
       "    ssl_certificate_key ${CERT_KEY};\n"
       "    include /etc/letsencrypt/options-ssl-nginx.conf;\n"
       "    ssl_dhparam /etc/letsencrypt/ssl-dhparams.pem;\n"
       "    include ${BACKEND_CONF};\n"
       "    location / {\n"
       "        proxy_pass \\$mad_backend;\n"
       "        proxy_set_header Host               \\$host;\n"
       "        proxy_set_header X-Real-IP          \\$remote_addr;\n"
       "        proxy_set_header X-Forwarded-For    \\$proxy_add_x_forwarded_for;\n"
       "        proxy_set_header X-Forwarded-Proto  \\$scheme;\n"
       "    }\n"
       "    client_max_body_size 1024m;\n"
       "}\n"
       "EOF\n"
       "ln -sf \"${FRONT_AVAIL}\" \"${FRONT_ENABLED}\"\n"
       "log \"Чищу конфликты server_name с ${DOMAIN}\"\n"
       "shopt -s nullglob\n"
       "for f in /etc/nginx/sites-available/* /etc/nginx/sites-enabled/* /etc/nginx/conf.d/*.conf; do\n"
       "  [ -f \"$f\" ] || continue\n"
       "  case \"$f\" in\n"
       "    \"$FRONT_AVAIL\"|\"$FRONT_ENABLED\"|\"$LOCAL_AVAIL\"|\"$LOCAL_ENABLED\"|\"$BACKEND_CONF\") continue ;;\n"
       "  esac\n"
       "  if grep -Eq '^\\s*server_name\\s+.*(" << dq(C.server_name) << "|www\\." << dq(C.server_name) << ")\\b' \"$f\"; then\n"
       "    log \"→ Убираю ${DOMAIN} из $f\"\n"
       "    sed -ri '\n"
       "      /^\\s*server_name/{\n"
       "        s/[[:space:]]+" << dq(C.server_name) << "\\b//g;\n"
       "        s/[[:space:]]+www\\." << dq(C.server_name) << "\\b//g;\n"
       "        s/server_name[[:space:]]*;$/server_name _;/;\n"
       "      }' \"$f\"\n"
       "  fi\n"
       "done\n"
       "shopt -u nullglob\n"
       "if ! grep -Eq 'include\\s+/etc/nginx/sites-enabled/\\*;' /etc/nginx/nginx.conf; then\n"
       "  log \"Добавляю include /etc/nginx/sites-enabled/*; в nginx.conf (внутрь http{})\"\n"
       "  sed -ri '/http\\s*\\{/a\\    include /etc/nginx/sites-enabled/*;' /etc/nginx/nginx.conf\n"
       "fi\n"
       "log \"Проверяю конфиг nginx\"; nginx -t\n"
       "log \"Перегружаю nginx\"; systemctl reload nginx\n"
       "if ss -lntp 2>/dev/null | grep -q ':8081 '; then log \"Локальный backend 8081 слушается\"; else log \"⚠️  8081 не слушается — проверь ${LOCAL_AVAIL} и error_log\"; fi\n"
       "current_backend=\"$(awk '/^\\s*set\\s+\\$mad_backend/{print $3}' \"${BACKEND_CONF}\" 2>/dev/null | tr -d ' ;' || true)\"; log \"Текущий backend: ${current_backend:-unknown}\"\n"
       "if command -v openssl >/dev/null 2>&1; then\n"
       "  log \"Проверяю SNI-сертификат для ${DOMAIN}\"\n"
       "  openssl s_client -connect 127.0.0.1:443 -servername \"${DOMAIN}\" </dev/null 2>/dev/null | openssl x509 -noout -subject -issuer | sed 's/^/   /'\n"
       "fi\n"
       "log \"Готово. Переключение:\"\n"
       "echo \"  sudo $0 local   # прокси на 127.0.0.1:8081\"\n"
       "echo \"  sudo $0 remote  # прокси на 192.168.88.198\"\n"
    << "EOS\n"
    << "  chmod 700 /root/setup_madmentat_nginx.sh\n"
    << "else\n"
    << "  echo \"✅ Нашёл /root/setup_madmentat_nginx.sh\" 1>&2;\n"
    << "fi\n"

    << "echo \"🔀 Запускаю переключение фронта на remote\" 1>&2;\n"
    << "/root/setup_madmentat_nginx.sh remote || { echo \"❌ Ошибка переключения\" 1>&2; exit 1; }\n"

    << "echo \"🐶 Обновляю /webserver/wachdog\" 1>&2;\n"
    << "mkdir -p /webserver || true\n"
    << "WD=/webserver/wachdog\n"
    << "if [ -f \"$WD\" ]; then\n"
    << "  if grep -Eq '^madmentat\\.ru_update=false\\b' \"$WD\"; then\n"
    << "    sed -ri 's/^madmentat\\.ru_update=false\\b/madmentat.ru_update=true/' \"$WD\"\n"
    << "    echo \"   → madmentat.ru_update=true (из false)\" 1>&2;\n"
    << "  else\n"
    << "    if ! grep -Eq '^madmentat\\.ru_update=' \"$WD\"; then echo 'madmentat.ru_update=true' >> \"$WD\"; echo \"   → добавил madmentat.ru_update=true\" 1>&2; else echo \"   → уже true или отсутствует false\" 1>&2; fi\n"
    << "  fi\n"
    << "else\n"
    << "  echo 'madmentat.ru_update=true' > \"$WD\"\n"
    << "  echo \"   → создан $WD с madmentat.ru_update=true\" 1>&2;\n"
    << "fi\n"

    << "echo \"——— Итог ———\" 1>&2;\n"
    << "echo \"✅ Импорт БД:       ${DB_IMPORTED}\" 1>&2;\n"
    << "echo \"📊 Таблиц в БД:     ${TABLES_AFTER}\" 1>&2;\n"
    << "echo \"✅ Скрипт nginx:     /root/setup_madmentat_nginx.sh\" 1>&2;\n"
    << "echo \"✅ Фронт:            remote (см. /etc/nginx/conf.d/" << dq(C.server_name) << ".backend.conf)\" 1>&2;\n"
    << "echo \"✅ Вачдог:           $(grep -E '^madmentat\\.ru_update=' \"$WD\" || echo '-')\" 1>&2;\n";

    const std::string sudopw   = C.remote_sudo_pass.empty() ? C.remote_pass : C.remote_sudo_pass;
    const std::string pass_b64 = base64_encode(sudopw);
    std::ostringstream wrapper;
    wrapper
        << "PW_B64='" << pass_b64 << "'; "
        << "if sudo -n true 2>/dev/null; then "
            "sudo -p '' bash -se <<'ROOT'\n"
        << root.str()
        << "ROOT\n"
        << "else "
            "( printf '%s\\n' \"$(printf '%s' \"$PW_B64\" | base64 -d)\"; cat <<'ROOT'\n"
        << root.str()
        << "ROOT\n"
        << ") | sudo -S -p '' bash -se; "
        << "fi";
    return wrapper.str();
}

// --------------------------- Команда развёртывания (APACHE) ---------------------------
static std::string build_apache_deploy_cmd(const Config& C,
                                           const std::string& remote_tar,
                                           const std::string& remote_sql,
                                           const std::string& remote_day) {
    const std::string conf_avail = "/etc/apache2/sites-available/" + C.server_name + ".local.conf";
    std::string php_sock = C.php_fpm_sock.empty()
        ? ("/run/php/php" + C.php_version + "-fpm.sock")
        : C.php_fpm_sock;
    std::ostringstream tpl;
    tpl
    << "<VirtualHost *:" << C.local_http_port << ">\n"
    << "    ServerName " << C.server_name << "\n"
    << "    DocumentRoot " << C.remote_site_dir << "\n"
    << "    <Directory " << C.remote_site_dir << ">\n"
    << "        Options Indexes FollowSymLinks\n"
    << "        AllowOverride All\n"
    << "        Require all granted\n"
    << "    </Directory>\n"
    << "    ErrorLog ${APACHE_LOG_DIR}/" << C.server_name << "_error.log\n"
    << "    CustomLog ${APACHE_LOG_DIR}/" << C.server_name << "_access.log combined\n"
    << "    <FilesMatch \\.php$>\n"
    << "        SetHandler \"proxy:unix:" << php_sock << "|fcgi://localhost/\"\n"
    << "    </FilesMatch>\n"
    << "</VirtualHost>\n";

    std::ostringstream cmd;
    cmd
    << "set -e; "
    << "SUDO='sudo -n'; if ! $SUDO true 2>/dev/null; then SUDO=\"echo '"
    << sh_escape_single(C.remote_sudo_pass.empty() ? C.remote_pass : C.remote_sudo_pass)
    << "' | sudo -S -p ''\"; fi; "
    << "echo '→ Проверка окружения (apache2/php-fpm/mysql/tar)'; "
    << "command -v apache2ctl >/dev/null || { echo '❌ apache2ctl не найден'; exit 1; }; "
    << "command -v tar        >/dev/null || { echo '❌ tar не установлен'; exit 1; }; "
    << "command -v mysql      >/dev/null || { echo '❌ mysql клиент не установлен'; exit 1; }; "
    << "if ! command -v php-fpm >/dev/null && ! command -v php-fpm" << C.php_version << " >/dev/null; then "
         "echo '❌ PHP-FPM не найден'; exit 1; "
       "fi; "
    << "PHP_SOCK='" << php_sock << "'; "
    << "[ -S \"$PHP_SOCK\" ] || { echo \"❌ Нет сокета PHP-FPM: $PHP_SOCK\"; ls -l /run/php || true; exit 1; }; "
    << "echo '→ Обновление рабочей копии'; "
    << "mkdir -p '" << C.remote_site_dir << "'; "
    << "rm -rf '" << C.remote_site_dir << "'.old; "
    << "mv '" << C.remote_site_dir << "' '" << C.remote_site_dir << ".old' 2>/dev/null || true; "
    << "mkdir -p '" << C.remote_site_dir << "'; "

    << "echo '→ Распаковка сайта (прогресс)'; "
    << "if command -v pv >/dev/null 2>&1; then "
         "pv -f -p -t -e -r -b '" << remote_tar << "' | tar -xzf - -C '" << C.remote_site_dir << "'; "
       "else "
         "echo '   pv не найден — индикатор точками'; "
         "tar -xzf '" << remote_tar << "' -C '" << C.remote_site_dir << "' --checkpoint=500 --checkpoint-action=echo=. ; echo; "
       "fi; "

    << "echo '→ Права'; "
    << "$SUDO /bin/chown -R www-data:www-data '" << C.remote_site_dir << "' || true; "
    << "find '" << C.remote_site_dir << "' -type d -exec /bin/chmod 755 {} \\; || true; "
    << "find '" << C.remote_site_dir << "' -type f -exec /bin/chmod 644 {} \\; || true; "

    << "echo '→ Apache vhost'; "
    << "TMPCONF=$(mktemp) && printf '%s' '" << sh_escape_single(tpl.str()) << "' > \"$TMPCONF\" && "
       "$SUDO /bin/mv \"$TMPCONF\" '" << conf_avail << "'; "
    << "$SUDO /usr/sbin/a2enmod proxy proxy_fcgi setenvif rewrite >/dev/null || true; "
    << "$SUDO /usr/sbin/a2ensite '" << C.server_name << ".local.conf' >/dev/null || true; "
    << "echo '→ apache2ctl configtest'; $SUDO /usr/sbin/apache2ctl configtest; "

    << "echo '→ Подготовка БД и пользователя (через root)'; "
    << "$SUDO /usr/bin/mysql -uroot -e \""
         "CREATE DATABASE IF NOT EXISTS \\`" << C.db_name << "\\` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci; "
         "CREATE USER IF NOT EXISTS '" << C.db_user << "'@'localhost' IDENTIFIED BY '" << C.db_pass << "'; "
         "CREATE USER IF NOT EXISTS '" << C.db_user << "'@'127.0.0.1' IDENTIFIED BY '" << C.db_pass << "'; "
         "GRANT ALL ON \\`" << C.db_name << "\\`.* TO "
             "'" << C.db_user << "'@'localhost', "
             "'" << C.db_user << "'@'127.0.0.1'; "
         "FLUSH PRIVILEGES;\"; "

    << "echo '→ Бэкап текущей БД на резерве (мягко)'; "
    << "$SUDO /usr/bin/mysqldump " << C.db_name << " > '" << remote_day << "/db_old.sql' 2>/dev/null || true; "

    << "echo '→ Импорт новой БД (через root)'; "
    << "$SUDO /usr/bin/mysql " << C.db_name << " < '" << remote_sql << "'; "

    << "echo '→ Перезапуск apache2'; $SUDO /bin/systemctl restart apache2; "
    << "echo '→ Ротация'; "
    << "find '" << C.remote_backup_base << "' -maxdepth 1 -type d "
       "-regextype posix-extended -regex '.*/[0-9]{4}-[0-9]{2}-[0-9]{2}$' -mtime +7 -exec rm -rf {} + ; "
    << "echo '✓ Apache: развёртывание завершено';";
    return cmd.str();
}

// --------------------------- Один «рабочий прогон» ---------------------------
static int run_once(const Config& cfg) {
    std::cout << "📁 Локальный сайт: " << cfg.local_site_dir << "\n";
    std::cout << "🛢️  База: " << cfg.db_name << " (user: " << cfg.db_user << ")\n";
    std::cout << "🌐 Резерв: " << cfg.remote_user << "@" << cfg.remote_host << ":" << cfg.remote_site_dir
              << " (" << cfg.target_server << ")\n";
    std::cout << "🔄 Локальные порты сайта: " << cfg.local_http_port
              << (cfg.local_https_port>0?("/"+std::to_string(cfg.local_https_port)):"")
              << " | switch_to_local=" << (cfg.switch_to_local ? "yes":"no") << "\n";

    if (!fs::exists(cfg.local_site_dir)) { std::cerr << "❌ Нет каталога: " << cfg.local_site_dir << "\n"; return 1; }

    // Артефакты (каждый прогон — своя дата)
    const std::string date = today();
    const std::string tmp_dir  = "/tmp/madbackuper_" + date;
    const std::string tar_path = tmp_dir + "/site_" + date + ".tar.gz";
    const std::string sql_path = tmp_dir + "/db_"   + date + ".sql";
    const std::string cnf_path = tmp_dir + "/db.cnf";
    fs::create_directories(tmp_dir);

    // DB creds (0600)
    { std::ofstream cnf(cnf_path); cnf << "[client]\nuser=" << cfg.db_user << "\npassword=" << cfg.db_pass << "\n";
      cnf.close(); chmod(cnf_path.c_str(), 0600); }

    // Архивация
    std::cout << "📦 Архивация сайта...\n";
    if (cfg.skip_tar && fs::exists(tar_path) && fs::file_size(tar_path) > 0) {
        std::cout << "⏭ --skip-tar: найден готовый архив: " << tar_path
                  << " (" << human_size((uint64_t)fs::file_size(tar_path)) << "), пропускаю упаковку.\n";
    } else {
        if (cfg.skip_tar) std::cout << "⚠️  --skip-tar запрошен, но архив не найден — создаю новый.\n";
        uint64_t total_bytes = dir_size_bytes(cfg.local_site_dir);
        std::cout << "   Размер каталога: ~" << human_size(total_bytes) << "\n";
        int rc_archive = 0;
        if (has_command("pv")) {
            std::cout << "   Использую pv для прогресса…\n";
            std::ostringstream cmd; cmd << "tar -C '" << cfg.local_site_dir << "' -cf - . | pv -s " << total_bytes
                                        << " | gzip > '" << tar_path << "'";
            rc_archive = run_local(cmd.str());
        } else {
            std::cout << "   ℹ️ pv не найден — покажу спиннер (sudo apt install pv)\n";
            std::ostringstream cmd; cmd << "tar -czf '" << tar_path << "' -C '" << cfg.local_site_dir << "' .";
            rc_archive = run_with_spinner(cmd.str(), "Архивация");
        }
        if (rc_archive != 0) { unlink(cnf_path.c_str()); return 1; }
        std::cout << "✅ Архив: " << tar_path << "\n";
    }

    // Дамп БД
    std::cout << "🛢️  Дамп базы...\n";
    if (cfg.skip_sql && fs::exists(sql_path) && fs::file_size(sql_path) > 0) {
        std::cout << "⏭ --skip-sql: найден существующий дамп: " << sql_path << "\n";
    } else {
        if (cfg.skip_sql) std::cout << "⚠️  --skip-sql запрошен, но дамп не найден — делаю новый.\n";
        std::string cmd = "mysqldump --defaults-extra-file='" + cnf_path + "' "
                          "--single-transaction --quick --routines --triggers --events --no-tablespaces "
                          + cfg.db_name + " > '" + sql_path + "' 2> '" + tmp_dir + "/mysqldump.err'";
        if (run_local(cmd, /*echo=*/false) != 0) { unlink(cnf_path.c_str()); return 1; }
        std::cout << "✅ Дамп: " << sql_path << "\n";
    }
    unlink(cnf_path.c_str());

    // SSH-сессия
    ssh_session session = ssh_new();
    if (!session) { std::cerr << "❌ ssh_new\n"; return 1; }
    int timeout = 30;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(session, SSH_OPTIONS_HOST, cfg.remote_host.c_str());
    ssh_options_set(session, SSH_OPTIONS_USER, cfg.remote_user.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &cfg.ssh_port);
    std::cout << "🔌 Подключаемся к " << cfg.remote_host << ":" << cfg.ssh_port << "...\n";
    if (ssh_connect(session) != SSH_OK) { std::cerr << "❌ ssh_connect: " << ssh_get_error(session) << "\n"; ssh_free(session); return 1; }
    std::cout << "✅ Соединение установлено (" << peer_ip(session) << ")\n";
    if (ssh_userauth_password(session, nullptr, cfg.remote_pass.c_str()) != SSH_AUTH_SUCCESS) {
        std::cerr << "❌ Аутентификация: " << ssh_get_error(session) << "\n"; ssh_disconnect(session); ssh_free(session); return 1;
    }

    // Подготовка места
    uint64_t need_bytes = 0; try { need_bytes += fs::file_size(tar_path); } catch (...) {}
                             try { need_bytes += fs::file_size(sql_path); } catch (...) {}
    if (ensure_space_and_bind_mount(session, cfg, need_bytes) != 0) { ssh_disconnect(session); ssh_free(session); return 1; }

    // Bootstrap
    if (bootstrap_remote(session, cfg) != 0) std::cerr << "⚠️  Подготовка удалёнки с предупреждениями.\n";

    // SFTP
    sftp_session sftp = sftp_new(session);
    if (!sftp) { std::cerr << "❌ sftp_new\n"; ssh_disconnect(session); ssh_free(session); return 1; }
    if (sftp_init(sftp) != SSH_OK) { std::cerr << "❌ sftp_init: " << ssh_get_error(session) << "\n"; sftp_free(sftp); ssh_disconnect(session); ssh_free(session); return 1; }

    const std::string remote_day   = cfg.remote_backup_base + "/" + date;
    const std::string remote_tar   = remote_day + "/site_" + date + ".tar.gz";
    const std::string remote_sql   = remote_day + "/db_"   + date + ".sql";

    std::cout << "📂 Готовим удалённую папку: " << remote_day << "\n";
    if (sftp_mkdirs(session, sftp, remote_day) != SSH_OK) {
        std::cerr << "❌ Не удалось создать каталог на удалёнке (SFTP)\n";
        sftp_free(sftp); ssh_disconnect(session); ssh_free(session); return 1;
    }

    auto remote_file_nonzero = [&](const std::string& path)->bool{
        sftp_attributes st = sftp_stat(sftp, path.c_str());
        if (!st) return false;
        bool ok = (st->type == SSH_FILEXFER_TYPE_REGULAR && st->size > 0);
        sftp_attributes_free(st);
        return ok;
    };

    // Передача (с учётом --skip-upload)
    bool need_upload_tar = true, need_upload_sql = true;
    if (cfg.skip_upload) {
        bool has_tar = remote_file_nonzero(remote_tar);
        bool has_sql = remote_file_nonzero(remote_sql);
        if (has_tar && has_sql) {
            std::cout << "⏭ --skip-upload: на удалёнке уже есть архив и дамп за сегодня, пропускаю загрузку.\n";
            need_upload_tar = need_upload_sql = false;
        } else {
            if (!has_tar) std::cout << "→ На удалёнке нет архива — придётся загрузить.\n";
            if (!has_sql) std::cout << "→ На удалёнке нет дампа — придётся загрузить.\n";
        }
    }
    if (need_upload_tar) {
        std::cout << "🚚 Отправка архива...\n";
        int sftp_err = SSH_FX_OK;
        if (sftp_upload_file_progress(session, sftp, tar_path, remote_tar, "Архив", &sftp_err) != 0) {
            std::cerr << "⚠️  SFTP-сбой (err=" << sftp_err << " " << sftp_errname(sftp_err)
                      << "). Перехожу на SSH-поток.\n";
            if (ssh_stream_upload(session, tar_path, remote_tar, "Архив(ssh)") != 0) {
                std::cerr << "❌ Передача архива по SSH тоже не удалась\n";
                sftp_free(sftp); ssh_disconnect(session); ssh_free(session); return 1;
            }
        }
    } else {
        std::cout << "⏭ Архив: пропущено (файл уже на удалёнке)\n";
    }
    if (need_upload_sql) {
        std::cout << "🚚 Отправка дампа БД...\n";
        int sftp_err = SSH_FX_OK;
        if (sftp_upload_file_progress(session, sftp, sql_path, remote_sql, "Дамп  ", &sftp_err) != 0) {
            std::cerr << "⚠️  SFTP-сбой (err=" << sftp_err << " " << sftp_errname(sftp_err)
                      << "). Перехожу на SSH-поток.\n";
            if (ssh_stream_upload(session, sql_path, remote_sql, "Дамп(ssh)") != 0) {
                std::cerr << "❌ Передача дампа по SSH тоже не удалась\n";
                sftp_free(sftp); ssh_disconnect(session); ssh_free(session); return 1;
            }
        }
    } else {
        std::cout << "⏭ Дамп: пропущено (файл уже на удалёнке)\n";
    }

    // Развёртывание
    std::cout << "🧩 Развёртывание на удалённом хосте (" << cfg.target_server << ")...\n";
    std::string deploy_cmd = (cfg.target_server == "nginx")
                           ? build_nginx_deploy_cmd(cfg, remote_tar, remote_sql, remote_day)
                           : build_apache_deploy_cmd(cfg, remote_tar, remote_sql, remote_day);
    if (ssh_exec(session, deploy_cmd, true) != 0)
        std::cerr << "⚠️  Развёртывание завершилось с ошибкой. Проверь вывод выше.\n";

    // Завершение
    sftp_free(sftp);
    ssh_disconnect(session);
    ssh_free(session);

    std::cout << "\n🎉 Бэкап и передача завершены.\n";
    std::cout << "ℹ️ Файлы на удалёнке: " << remote_day << "\n";
    return 0;
}

// --------------------------- MAIN ---------------------------
int main(int argc, char** argv) {
    std::cout << "🔧 madbackuper: SCP/SFTP бэкап и развертывание\n";

    // Флаги демона
    bool daemon_mode = false;
    int  alarm_h = 3, alarm_m = 0; // по умолчанию 03:00

    // Конфиг: создать при первом запуске
    std::string cfg_path = CFG_PATH_PRIMARY;
    if (!fs::exists(cfg_path)) {
        std::cerr << "ℹ️ Конфиг не найден: " << cfg_path << " — генерирую по умолчанию...\n";
        write_default_config(cfg_path);
        if (!fs::exists(cfg_path)) {
            cfg_path = CFG_PATH_FALLBACK;
            std::cerr << "⚠️ Нет прав на /etc. Пишу конфиг сюда: " << cfg_path << "\n";
            write_default_config(cfg_path);
            if (!fs::exists(cfg_path)) { std::cerr << "❌ Не удалось создать конфиг\n"; return 1; }
        }
        std::cout << "✅ Создан конфиг: " << cfg_path << "\n";
        std::cout << "⚠️ Отредактируй его при необходимости и запусти программу снова.\n";
        return 0;
    }

    // Загрузка конфига + CLI
    Config cfg;
    load_kv_file(cfg_path, cfg);
    apply_cli_kv(argc, argv, cfg, daemon_mode, alarm_h, alarm_m);

    // Валидация
    std::string verr; if (!validate(cfg, verr)) { std::cerr << "❌ Ошибка параметров: " << verr << "\n"; return 1; }

    // Сигналы для аккуратной остановки
    std::signal(SIGINT,  on_sigint);
    std::signal(SIGTERM, on_sigterm);

    // Режим демона
    if (daemon_mode) {
        std::cout << "🕒 Демон-режим: запуск в "
                  << (alarm_h<10?"0":"") << alarm_h << ":" << (alarm_m<10?"0":"") << alarm_m
                  << " по локальному времени сервера.\n";
        while (!g_stop) {
            std::time_t t = next_local_time(alarm_h, alarm_m);
            char buf[32]; std::tm lt{}; localtime_r(&t, &lt);
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &lt);
            std::cout << "⏳ Жду до: " << buf << std::endl;

            sleep_until_epoch(t);
            if (g_stop) break;

            try {
                if (run_once(cfg) != 0)
                    std::cerr << "⚠️ run_once завершился с ошибкой\n";
            } catch (const std::exception& e) {
                std::cerr << "❌ Исключение в run_once: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "❌ Неизвестная ошибка в run_once\n";
            }

            // Защита от «двойного» выстрела в ту же минуту
            for (int i=0; i<60 && !g_stop; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::cout << "👋 Демон аккуратно остановлен.\n";
        return 0;
    }

    // Обычный «один раз и вышел»
    return run_once(cfg);
}
