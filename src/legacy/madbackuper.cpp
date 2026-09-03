// madbackuper.cpp
// ------------------------------------------------------------
// Бэкап и развёртывание на резервный хост (Ubuntu/Debian).
// Новое:
//   • Мультисайтовая архитектура: секции [site:...], [mirror:...] (см. include/mad/core.hpp)
//   • --skip-tar / --skip-sql / --skip-upload
//   • Проверка наличия nginx без зависимости от PATH
//   • Привилегированные действия через sudo (учёт отдельного remote_sudo_pass)
//   • Nginx: systemctl restart/reload, детальные проверки vhost (nginx -T)
//   • БД (MariaDB/MySQL): лог импорта и число таблиц после заливки
//   • Прогресс архивации/передачи, проверка места, bind-mount /webserver
//   • Автосоздание sudoers и нужных каталогов
//   • Прямая генерация локального backend-конфига nginx
//     (/etc/nginx/sites-available/<server>.local.conf, listen <local_http_port>)
//     — публичный фронт живёт на отдельном proxy
//
// Сборка:
//   g++ -std=c++17 madbackuper.cpp -o madbackuper -lssh
//
// Примеры:
//   ./madbackuper --skip-upload
//   ./madbackuper --site=site1 --mirror=mirror2
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

// --- ⬇ перенесено в модули: // --------------------------- Константы ---------------------------
#if 0
#endif
// --- ⬇ перенесено в модули: // --------------------------- Конфиг ---------------------------
#if 0
#endif
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

// --- ⬇ перенесено в модули: // --------------------------- SSH helpers ---------------------------
#if 0
#endif
// --- ⬇ перенесено в модули: // --------------------------- Конфиг I/O ---------------------------
#if 0
#endif
// --- ⬇ перенесено в модули: // --------------------------- Проверка параметров ---------------------------
#if 0
#endif
// --- ⬇ перенесено в модули: // --------------------------- SFTP helpers ---------------------------
#if 0
#endif
// --------------------------- sudo helpers ---------------------------
static std::string sh_escape_single(const std::string& s) {
    std::string out; out.reserve(s.size()+8);
    for (char c: s) { if (c=='\'') out += "'\\''"; else out += c; }
    return out;
}
static std::string sudo_prefix(ssh_session session, const Mirror& M) {
    if (M.remote_user == "root") return "";
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
    std::string sudopw = M.remote_sudo_pass.empty() ? M.remote_pass : M.remote_sudo_pass;
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
static int ensure_space_and_bind_mount(ssh_session session, const Mirror& M, uint64_t need_bytes) {
    std::cout << "💽 Проверка свободного места на удалёнке...\n";
    uint64_t avail = remote_bytes_avail(session, "/webserver");
    if (avail == 0) {
        std::string SUDO = sudo_prefix(session, M);
        ssh_exec(session, (SUDO.empty()? "" : (SUDO + " ")) + "mkdir -p /webserver", false);
        avail = remote_bytes_avail(session, "/webserver");
    }
    uint64_t headroom = std::max<uint64_t>(need_bytes / 10, 512ULL*1024*1024); // 10% или 512MB
    uint64_t need_total = need_bytes + headroom;
    std::cout << "   Нужно ~" << human_size(need_total) << ", доступно ~" << human_size(avail) << "\n";
    if (avail >= need_total) { std::cout << "✅ Места достаточно на текущем разделе.\n"; return 0; }
    std::cout << "⚠️  Места недостаточно — bind-mount $HOME/webserver -> /webserver...\n";
    std::string SUDO = sudo_prefix(session, M);
    std::string home;
    {
        std::string out;
        if (ssh_exec_capture(session, "printf %s \"$HOME\"", out, nullptr) == 0 && !trim(out).empty()) home = trim(out);
        else {
            std::string out2;
            if (ssh_exec_capture(session, "getent passwd \"$USER\" | cut -d: -f6", out2, nullptr) == 0 && !trim(out2).empty())
                home = trim(out2);
        }
        if (home.empty()) home = "/home/" + M.remote_user;
    }
    std::string prep =
        (SUDO.empty()? "" : (SUDO + " ")) + "mkdir -p '" + home + "/webserver' /webserver && "
        + (M.remote_user=="root" ? "true" :
           (SUDO.empty()? "" : (SUDO + " ")) + "chown -R " + M.remote_user + ":" + M.remote_user + " '" + home + "/webserver'") + " && "
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
static int bootstrap_remote(ssh_session session, const Mirror& M) {
    std::cout << "🛠️  Подготовка удалённого хоста...\n";
    std::string SUDO = sudo_prefix(session, M);
    // sudoers
    if (M.remote_user != "root") {
        const std::string sudoers = "/etc/sudoers.d/madbackuper";
        std::ostringstream content;
        content << "Cmnd_Alias MADBACKUP_CMDS = "
                << "/usr/sbin/nginx, /usr/sbin/nginx -t, "
                << "/bin/systemctl reload nginx, /bin/systemctl restart nginx, "
                << "/usr/sbin/apache2ctl, /bin/systemctl reload apache2, /bin/systemctl restart apache2, "
                << "/usr/bin/tee, /bin/mkdir, /bin/chown, /bin/chmod, /bin/ln, /bin/cp, /bin/mv, /bin/tar, /usr/bin/find, "
                << "/bin/mount, /bin/umount, /bin/mountpoint, /usr/bin/grep, /usr/bin/cut, /usr/bin/getent, "
                << "/usr/bin/mysql, /usr/bin/mysqldump\n"
                << M.remote_user << " ALL=(root) NOPASSWD: MADBACKUP_CMDS\n";
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
            << "mkdir -p '" << M.remote_backup_base << "' '" << M.remote_site_dir << "' && ";
        if (M.remote_user != "root") {
            std::string chown_cmd = (SUDO.empty()? "" : (SUDO + " "));
            chown_cmd += "chown -R " + M.remote_user + ":" + M.remote_user + " '" + M.remote_backup_base + "'";
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
static std::string build_nginx_deploy_cmd(const Site& S, const Mirror& M,
                                          const std::string& remote_tar,
                                          const std::string& remote_sql,
                                          const std::string& remote_day)
{
    auto dq = [](const std::string& s) {
        std::string r; r.reserve(s.size()*2);
        for (char c : s) { if (c == '\\' || c == '"') r.push_back('\\'); r.push_back(c); }
        return r;
    };
    const std::string php_sock = M.php_fpm_sock.empty()
        ? ("/run/php/php" + M.php_version + "-fpm.sock")
        : M.php_fpm_sock;

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
    << "mkdir -p \"" << dq(M.remote_site_dir) << "\"\n"
    << "rm -rf \"" << dq(M.remote_site_dir) << ".old\"\n"
    << "mv \"" << dq(M.remote_site_dir) << "\" \"" << dq(M.remote_site_dir) << ".old\" 2>/dev/null || true\n"
    << "mkdir -p \"" << dq(M.remote_site_dir) << "\"\n"

    << "echo \"📤 Распаковка архива сайта\" 1>&2;\n"
    << "if command -v pv >/dev/null 2>&1; then "
         "pv -f -p -t -e -r -b \"" << dq(remote_tar) << "\" | tar -xzf - -C \"" << dq(M.remote_site_dir) << "\"; "
       "else "
         "tar -xzf \"" << dq(remote_tar) << "\" -C \"" << dq(M.remote_site_dir) << "\" --checkpoint=500 --checkpoint-action=echo=. ; echo; "
       "fi\n"

    << "REF=$(mktemp); touch \"$REF\"; "
       "find \"" << dq(M.remote_site_dir) << "\" \\( -type f -o -type d \\) -newer \"$REF\" -print0 | xargs -0 -r touch -r \"$REF\"; "
       "rm -f \"$REF\"\n"

    << "echo \"🧰 Выставляю права\" 1>&2;\n"
    << "chown -R www-data:www-data \"" << dq(M.remote_site_dir) << "\" || true\n"
    << "find \"" << dq(M.remote_site_dir) << "\" -type d -exec chmod 755 {} \\; || true\n"
    << "find \"" << dq(M.remote_site_dir) << "\" -type f -exec chmod 644 {} \\; || true\n"

    // БД
    << "echo \"🗄️  Подготавливаю БД и пользователя (MariaDB)\" 1>&2;\n"
    << "/usr/bin/mysql -uroot <<\\SQL\n"
       "CREATE DATABASE IF NOT EXISTS `" << S.db_name << "` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;\n"
       "CREATE USER IF NOT EXISTS '" << S.db_user << "'@'localhost' IDENTIFIED BY '" << S.db_pass << "';\n"
       "CREATE USER IF NOT EXISTS '" << S.db_user << "'@'127.0.0.1' IDENTIFIED BY '" << S.db_pass << "';\n"
       "GRANT ALL ON `" << S.db_name << "`.* TO '" << S.db_user << "'@'localhost';\n"
       "GRANT ALL ON `" << S.db_name << "`.* TO '" << S.db_user << "'@'127.0.0.1';\n"
       "FLUSH PRIVILEGES;\n"
    "SQL\n"
    << "echo \"📦 Бэкап прежней БД (мягко)\" 1>&2;\n"
    << "/usr/bin/mysqldump \"" << dq(S.db_name) << "\" > \"" << dq(remote_day) << "/db_old.sql\" 2>/dev/null || true\n"
    << "DB_IMPORTED=0; TABLES_AFTER=0;\n"
    << "if [ -s \"" << dq(remote_sql) << "\" ]; then "
         "echo \"⬇️  Импортирую дамп\" 1>&2; "
         "/usr/bin/mysql \"" << dq(S.db_name) << "\" < \"" << dq(remote_sql) << "\" && DB_IMPORTED=1; "
       "else "
         "echo \"⚠️ Дамп не найден или пуст — пропуск\" 1>&2; "
       "fi\n"
    << "TABLES_AFTER=$(/usr/bin/mysql -NBe \"SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='"
      << dq(S.db_name) << "'\" 2>/dev/null || echo 0)\n"
    << "echo \"   📊 Таблиц в БД после импорта: $TABLES_AFTER\" 1>&2;\n"

    // Локальный backend-конфиг (публичный фронт — на отдельном proxy)
    << "echo \"🧩 Пишу локальный backend-конфиг nginx\" 1>&2;\n"
    << "LOCAL_AVAIL=\"/etc/nginx/sites-available/" << dq(S.server_name) << ".local.conf\"\n"
    << "LOCAL_ENABLED=\"/etc/nginx/sites-enabled/" << dq(S.server_name) << ".local.conf\"\n"
    << "cat > \"$LOCAL_AVAIL\" <<'NGINX_LOCAL'\n"
    << "server {\n"
    << "    listen " << M.local_http_port << ";\n"
    << "    server_name " << S.server_name << ";\n"
    << "    root " << M.remote_site_dir << ";\n"
    << "    index index.php index.html;\n"
    << "    location ~ \\.php$ {\n"
    << "        fastcgi_pass unix:" << php_sock << ";\n"
    << "        fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;\n"
    << "        include fastcgi_params;\n"
    << "    }\n"
    << "}\n"
    << "NGINX_LOCAL\n"
    << "ln -sf \"$LOCAL_AVAIL\" \"$LOCAL_ENABLED\"\n"
    << "if ! grep -Eq 'include\\s+/etc/nginx/sites-enabled/\\*;' /etc/nginx/nginx.conf; then\n"
    << "  echo \"➕ Добавляю include /etc/nginx/sites-enabled/*; в nginx.conf (внутрь http{})\" 1>&2;\n"
    << "  sed -ri '/http\\s*\\{/a\\    include /etc/nginx/sites-enabled/*;' /etc/nginx/nginx.conf\n"
    << "fi\n"
    << "echo \"🔍 Проверяю конфиг nginx\" 1>&2; nginx -t\n"
    << "echo \"🔄 Перезагружаю nginx\" 1>&2; systemctl reload nginx\n"

    << "echo \"🧹 Ротация старых бэкапов (7+ дней)\" 1>&2;\n"
    << "find \"" << dq(M.remote_backup_base) << "\" -maxdepth 1 -type d -regextype posix-extended -regex '.*/[0-9]{4}-[0-9]{2}-[0-9]{2}$' -mtime +7 -exec rm -rf {} +\n"

    << "echo \"——— Итог ———\" 1>&2;\n"
    << "echo \"✅ Импорт БД:       ${DB_IMPORTED}\" 1>&2;\n"
    << "echo \"📊 Таблиц в БД:     ${TABLES_AFTER}\" 1>&2;\n"
    << "echo \"✅ Локальный backend: /etc/nginx/sites-available/" << dq(S.server_name) << ".local.conf (listen " << M.local_http_port << ")\" 1>&2;\n";

    const std::string sudopw   = M.remote_sudo_pass.empty() ? M.remote_pass : M.remote_sudo_pass;
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
static std::string build_apache_deploy_cmd(const Site& S, const Mirror& M,
                                           const std::string& remote_tar,
                                           const std::string& remote_sql,
                                           const std::string& remote_day) {
    const std::string conf_avail = "/etc/apache2/sites-available/" + S.server_name + ".local.conf";
    std::string php_sock = M.php_fpm_sock.empty()
        ? ("/run/php/php" + M.php_version + "-fpm.sock")
        : M.php_fpm_sock;
    std::ostringstream tpl;
    tpl
    << "Listen " << M.local_http_port << "\n"
    << "<VirtualHost *:" << M.local_http_port << ">\n"
    << "    ServerName " << S.server_name << "\n"
    << "    DocumentRoot " << M.remote_site_dir << "\n"
    << "    <Directory " << M.remote_site_dir << ">\n"
    << "        Options Indexes FollowSymLinks\n"
    << "        AllowOverride All\n"
    << "        Require all granted\n"
    << "    </Directory>\n"
    << "    ErrorLog ${APACHE_LOG_DIR}/" << S.server_name << "_error.log\n"
    << "    CustomLog ${APACHE_LOG_DIR}/" << S.server_name << "_access.log combined\n"
    << "    <FilesMatch \\.php$>\n"
    << "        SetHandler \"proxy:unix:" << php_sock << "|fcgi://localhost/\"\n"
    << "    </FilesMatch>\n"
    << "</VirtualHost>\n";

    std::ostringstream cmd;
    cmd
    << "set -e; "
    << "SUDO='sudo -n'; if ! $SUDO true 2>/dev/null; then SUDO=\"echo '"
    << sh_escape_single(M.remote_sudo_pass.empty() ? M.remote_pass : M.remote_sudo_pass)
    << "' | sudo -S -p ''\"; fi; "
    << "echo '→ Проверка окружения (apache2/php-fpm/mysql/tar)'; "
    << "command -v apache2ctl >/dev/null || { echo '❌ apache2ctl не найден'; exit 1; }; "
    << "command -v tar        >/dev/null || { echo '❌ tar не установлен'; exit 1; }; "
    << "command -v mysql      >/dev/null || { echo '❌ mysql клиент не установлен'; exit 1; }; "
    << "if ! command -v php-fpm >/dev/null && ! command -v php-fpm" << M.php_version << " >/dev/null; then "
         "echo '❌ PHP-FPM не найден'; exit 1; "
       "fi; "
    << "PHP_SOCK='" << php_sock << "'; "
    << "[ -S \"$PHP_SOCK\" ] || { echo \"❌ Нет сокета PHP-FPM: $PHP_SOCK\"; ls -l /run/php || true; exit 1; }; "
    << "echo '→ Обновление рабочей копии'; "
    << "$SUDO /bin/mkdir -p '" << M.remote_site_dir << "'; "
    << "$SUDO /bin/rm -rf '" << M.remote_site_dir << "'.old; "
    << "$SUDO /bin/mv '" << M.remote_site_dir << "' '" << M.remote_site_dir << ".old' 2>/dev/null || true; "
    << "$SUDO /bin/mkdir -p '" << M.remote_site_dir << "'; "
    << "$SUDO /bin/chown " << M.remote_user << " '" << M.remote_site_dir << "'; "

    << "echo '→ Распаковка сайта (прогресс)'; "
    << "if command -v pv >/dev/null 2>&1; then "
         "pv -f -p -t -e -r -b '" << remote_tar << "' | tar -xzf - -C '" << M.remote_site_dir << "'; "
       "else "
         "echo '   pv не найден — индикатор точками'; "
         "tar -xzf '" << remote_tar << "' -C '" << M.remote_site_dir << "' --checkpoint=500 --checkpoint-action=echo=. ; echo; "
       "fi; "

    << "echo '→ Права'; "
    << "$SUDO /bin/chown -R www-data:www-data '" << M.remote_site_dir << "' || true; "
    << "find '" << M.remote_site_dir << "' -type d -exec $SUDO /bin/chmod 755 {} \\; || true; "
    << "find '" << M.remote_site_dir << "' -type f -exec $SUDO /bin/chmod 644 {} \\; || true; "

    << "echo '→ Apache vhost'; "
    << "TMPCONF=$(mktemp) && printf '%s' '" << sh_escape_single(tpl.str()) << "' > \"$TMPCONF\" && "
       "$SUDO /bin/mv \"$TMPCONF\" '" << conf_avail << "'; "
    << "$SUDO /bin/chmod 644 '" << conf_avail << "'; "
    << "$SUDO /usr/sbin/a2enmod proxy proxy_fcgi setenvif rewrite >/dev/null || true; "
    << "$SUDO /usr/sbin/a2ensite '" << S.server_name << ".local.conf' >/dev/null || true; "
    << "echo '→ apache2ctl configtest'; $SUDO /usr/sbin/apache2ctl configtest; "

    << "echo '→ Подготовка БД и пользователя (через root)'; "
    << "$SUDO /usr/bin/mysql -uroot -e \""
         "CREATE DATABASE IF NOT EXISTS \\`" << S.db_name << "\\` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci; "
         "CREATE USER IF NOT EXISTS '" << S.db_user << "'@'localhost' IDENTIFIED BY '" << S.db_pass << "'; "
         "CREATE USER IF NOT EXISTS '" << S.db_user << "'@'127.0.0.1' IDENTIFIED BY '" << S.db_pass << "'; "
         "GRANT ALL ON \\`" << S.db_name << "\\`.* TO "
             "'" << S.db_user << "'@'localhost', "
             "'" << S.db_user << "'@'127.0.0.1'; "
         "FLUSH PRIVILEGES;\"; "

    << "echo '→ Бэкап текущей БД на резерве (мягко)'; "
    << "$SUDO /usr/bin/mysqldump " << S.db_name << " > '" << remote_day << "/db_old.sql' 2>/dev/null || true; "

    << "echo '→ Импорт новой БД (через root)'; "
    << "$SUDO /usr/bin/mysql " << S.db_name << " < '" << remote_sql << "'; "

    << "echo '→ Перезапуск apache2'; $SUDO /bin/systemctl restart apache2; "
    << "echo '→ Ротация'; "
    << "find '" << M.remote_backup_base << "' -maxdepth 1 -type d "
       "-regextype posix-extended -regex '.*/[0-9]{4}-[0-9]{2}-[0-9]{2}$' -mtime +7 -exec rm -rf {} + ; "
    << "echo '✓ Apache: развёртывание завершено';";
    return cmd.str();
}

// --------------------------- Один «рабочий прогон» ---------------------------
static int run_once(const Site& S, const Mirror& M,
                    bool skip_tar, bool skip_sql, bool skip_upload) {
    std::cout << "📁 Локальный сайт: " << S.local_site_dir << "\n";
    std::cout << "🛢️  База: " << S.db_name << " (user: " << S.db_user << ")\n";
    std::cout << "🌐 Резерв: " << M.remote_user << "@" << M.remote_host << ":" << M.remote_site_dir
              << " (" << M.target_server << ")\n";
    std::cout << "🔄 Локальные порты сайта: " << M.local_http_port
              << (M.local_https_port>0?("/"+std::to_string(M.local_https_port)):"") << "\n";

    if (!fs::exists(S.local_site_dir)) { std::cerr << "❌ Нет каталога: " << S.local_site_dir << "\n"; return 1; }

    // Артефакты (каждый прогон — своя дата)
    const std::string date = today();
    const std::string tmp_dir  = "/tmp/madbackuper_" + date;
    const std::string tar_path = tmp_dir + "/site_" + date + ".tar.gz";
    const std::string sql_path = tmp_dir + "/db_"   + date + ".sql";
    const std::string cnf_path = tmp_dir + "/db.cnf";
    fs::create_directories(tmp_dir);

    // DB creds (0600)
    { std::ofstream cnf(cnf_path); cnf << "[client]\nuser=" << S.db_user << "\npassword=" << S.db_pass << "\n";
      cnf.close(); chmod(cnf_path.c_str(), 0600); }

    // Архивация
    std::cout << "📦 Архивация сайта...\n";
    if (skip_tar && fs::exists(tar_path) && fs::file_size(tar_path) > 0) {
        std::cout << "⏭ --skip-tar: найден готовый архив: " << tar_path
                  << " (" << human_size((uint64_t)fs::file_size(tar_path)) << "), пропускаю упаковку.\n";
    } else {
        if (skip_tar) std::cout << "⚠️  --skip-tar запрошен, но архив не найден — создаю новый.\n";
        uint64_t total_bytes = dir_size_bytes(S.local_site_dir);
        std::cout << "   Размер каталога: ~" << human_size(total_bytes) << "\n";
        int rc_archive = 0;
        if (has_command("pv")) {
            std::cout << "   Использую pv для прогресса…\n";
            std::ostringstream cmd; cmd << "tar -C '" << S.local_site_dir << "' -cf - . | pv -s " << total_bytes
                                        << " | gzip > '" << tar_path << "'";
            rc_archive = run_local(cmd.str());
        } else {
            std::cout << "   ℹ️ pv не найден — покажу спиннер (sudo apt install pv)\n";
            std::ostringstream cmd; cmd << "tar -czf '" << tar_path << "' -C '" << S.local_site_dir << "' .";
            rc_archive = run_with_spinner(cmd.str(), "Архивация");
        }
        if (rc_archive != 0) { unlink(cnf_path.c_str()); return 1; }
        std::cout << "✅ Архив: " << tar_path << "\n";
    }

    // Дамп БД
    std::cout << "🛢️  Дамп базы...\n";
    if (skip_sql && fs::exists(sql_path) && fs::file_size(sql_path) > 0) {
        std::cout << "⏭ --skip-sql: найден существующий дамп: " << sql_path << "\n";
    } else {
        if (skip_sql) std::cout << "⚠️  --skip-sql запрошен, но дамп не найден — делаю новый.\n";
        std::string cmd = "mysqldump --defaults-extra-file='" + cnf_path + "' "
                          "--single-transaction --quick --routines --triggers --events --no-tablespaces "
                          + S.db_name + " > '" + sql_path + "' 2> '" + tmp_dir + "/mysqldump.err'";
        if (run_local(cmd, /*echo=*/false) != 0) { unlink(cnf_path.c_str()); return 1; }
        std::cout << "✅ Дамп: " << sql_path << "\n";
    }
    unlink(cnf_path.c_str());

    // SSH-сессия
    ssh_session session = ssh_new();
    if (!session) { std::cerr << "❌ ssh_new\n"; return 1; }
    int timeout = 30;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(session, SSH_OPTIONS_HOST, M.remote_host.c_str());
    ssh_options_set(session, SSH_OPTIONS_USER, M.remote_user.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &M.ssh_port);
    std::cout << "🔌 Подключаемся к " << M.remote_host << ":" << M.ssh_port << "...\n";
    if (ssh_connect(session) != SSH_OK) { std::cerr << "❌ ssh_connect: " << ssh_get_error(session) << "\n"; ssh_free(session); return 1; }
    std::cout << "✅ Соединение установлено (" << peer_ip(session) << ")\n";
    if (ssh_userauth_password(session, nullptr, M.remote_pass.c_str()) != SSH_AUTH_SUCCESS) {
        std::cerr << "❌ Аутентификация: " << ssh_get_error(session) << "\n"; ssh_disconnect(session); ssh_free(session); return 1;
    }

    // Подготовка места
    uint64_t need_bytes = 0; try { need_bytes += fs::file_size(tar_path); } catch (...) {}
                             try { need_bytes += fs::file_size(sql_path); } catch (...) {}
    if (ensure_space_and_bind_mount(session, M, need_bytes) != 0) { ssh_disconnect(session); ssh_free(session); return 1; }

    // Bootstrap
    if (bootstrap_remote(session, M) != 0) std::cerr << "⚠️  Подготовка удалёнки с предупреждениями.\n";

    // SFTP
    sftp_session sftp = sftp_new(session);
    if (!sftp) { std::cerr << "❌ sftp_new\n"; ssh_disconnect(session); ssh_free(session); return 1; }
    if (sftp_init(sftp) != SSH_OK) { std::cerr << "❌ sftp_init: " << ssh_get_error(session) << "\n"; sftp_free(sftp); ssh_disconnect(session); ssh_free(session); return 1; }

    const std::string remote_day   = M.remote_backup_base + "/" + date;
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
    if (skip_upload) {
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
    std::cout << "🧩 Развёртывание на удалённом хосте (" << M.target_server << ")...\n";
    std::string deploy_cmd = (M.target_server == "nginx")
                           ? build_nginx_deploy_cmd(S, M, remote_tar, remote_sql, remote_day)
                           : build_apache_deploy_cmd(S, M, remote_tar, remote_sql, remote_day);
    int deploy_rc = ssh_exec(session, deploy_cmd, true);
    if (deploy_rc != 0)
        std::cerr << "⚠️  Развёртывание завершилось с ошибкой (код " << deploy_rc << "). Проверь вывод выше.\n";

    // Завершение
    sftp_free(sftp);
    ssh_disconnect(session);
    ssh_free(session);

    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
    std::cout << "🧹 Временные файлы удалены: " << tmp_dir << "\n";
    if (deploy_rc == 0) {
        std::cout << "\n🎉 Бэкап и передача завершены.\n";
    }
    std::cout << "ℹ️ Файлы на удалёнке: " << remote_day << "\n";
    return deploy_rc;
}

// --------------------------- MAIN ---------------------------
int main(int argc, char** argv,
         const std::string& site_sel   = "", // "" — первый сайт
         const std::string& mirror_sel = "") // "" — все зеркала сайта
{
    std::cout << "🔧 madbackuper: SCP/SFTP бэкап и развертывание\n";

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
    apply_cli_kv(argc, argv, cfg);

    // Валидация
    std::string verr; if (!validate(cfg, verr)) { std::cerr << "❌ Ошибка параметров: " << verr << "\n"; return 1; }

    // Сигналы для аккуратной остановки
    std::signal(SIGINT,  on_sigint);
    std::signal(SIGTERM, on_sigterm);

    // Выбор сайта
    const Site* S = nullptr;
    if (site_sel.empty()) {
        if (cfg.sites.empty()) { std::cerr << "❌ В конфиге нет сайтов.\n"; return 1; }
        S = &cfg.sites.front();
        std::cout << "🌍 Сайт: " << S->name << " (по умолчанию)\n";
    } else {
        for (const auto& s : cfg.sites) if (s.name == site_sel) { S = &s; break; }
        if (!S) { std::cerr << "❌ Сайт не найден: " << site_sel << "\n"; return 1; }
        std::cout << "🌍 Сайт: " << S->name << "\n";
    }

    // Выбор зеркал
    if (!mirror_sel.empty()) {
        bool found = false;
        for (const auto& s : cfg.sites)
            for (const auto& mname : s.mirrors)
                if (mname == mirror_sel) { found = true; break; }
        if (!found) { std::cerr << "❌ Зеркало не найдено: " << mirror_sel << "\n"; return 1; }
        bool in_site = false;
        for (const auto& mname : S->mirrors) if (mname == mirror_sel) { in_site = true; break; }
        if (!in_site) {
            std::cerr << "❌ Зеркало «" << mirror_sel << "» не привязано к сайту «" << S->name << "»\n";
            return 1;
        }
    }

    int overall_rc = 0;
    bool any = false;
    for (const std::string& mname : S->mirrors) {
        if (!mirror_sel.empty() && mname != mirror_sel) continue;
        const Mirror* M = nullptr;
        for (const auto& m : cfg.mirrors) if (m.name == mname) { M = &m; break; }
        if (!M) { std::cerr << "❌ Зеркало «" << mname << "» не найдено в [mirrors].\n"; overall_rc = 1; continue; }
        any = true;
        std::cout << "\n📦 Зеркало " << M->name << " (" << M->remote_host << ")...\n";
        int rc = run_once(*S, *M, cfg.skip_tar, cfg.skip_sql, cfg.skip_upload);
        if (rc != 0 && overall_rc == 0) overall_rc = rc;
    }
    if (!any) { std::cerr << "❌ Нет зеркал для обработки.\n"; return 1; }
    return overall_rc;
}
