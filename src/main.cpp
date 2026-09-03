#include <filesystem>
#include <string>
#include <iostream>

#include "mad/core.hpp"
#include "mad/net.hpp"
#include "mad/deploy.hpp"
#include "mad/daemon.hpp"
#include "mad/proxy.hpp"

// Дадим legacy доступ к именам без префикса mad::
using namespace mad;
using Config = mad::Config; // на случай, если где-то используется просто Config
namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Адаптер для старых версий legacy, где apply_cli_kv имела расширенную сигнатуру.
inline void apply_cli_kv(int argc, char** argv, mad::Config& cfg,
                         bool& /*daemon_mode*/, int& /*alarm_h*/, int& /*alarm_m*/) {
    mad::apply_cli_kv(argc, argv, cfg);
}

// ─── Изолируем версии функций, определённые в legacy, ────────────────────────
#define sh_escape_single              sh_escape_single__legacy
#define sudo_prefix                   sudo_prefix__legacy
#define remote_bytes_avail            remote_bytes_avail__legacy
#define ensure_space_and_bind_mount   ensure_space_and_bind_mount__legacy
#define bootstrap_remote              bootstrap_remote__legacy
#define peer_ip                       peer_ip__legacy
#define run_local                     run_local__legacy
#define run_with_spinner              run_with_spinner__legacy
#define build_nginx_deploy_cmd        build_nginx_deploy_cmd__legacy
#define build_apache_deploy_cmd       build_apache_deploy_cmd__legacy

// unity-включение legacy с переименованием main
#define main mad_legacy_main
#include "legacy/madbackuper.cpp"
#undef main

// Откатываем переименования (на будущее)
#undef sh_escape_single
#undef sudo_prefix
#undef remote_bytes_avail
#undef ensure_space_and_bind_mount
#undef bootstrap_remote
#undef peer_ip
#undef run_local
#undef run_with_spinner
#undef build_nginx_deploy_cmd
#undef build_apache_deploy_cmd
// ─────────────────────────────────────────────────────────────────────────────

static bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == flag) return true;
    return false;
}

static std::string arg_value(int argc, char** argv, const char* key) {
    const std::string p = std::string("--") + key + "=";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind(p, 0) == 0) return a.substr(p.size());
    }
    return {};
}

static void load_or_create_config(mad::Config& cfg) {
    std::string cfg_path = mad::CFG_PATH_PRIMARY;
    if (!fs::exists(cfg_path)) {
        try {
            mad::write_default_config(cfg_path);
        } catch (...) {
            cfg_path = mad::CFG_PATH_FALLBACK;
            if (!fs::exists(cfg_path)) {
                mad::write_default_config(cfg_path);
            }
        }
    }
    mad::load_kv_file(cfg_path, cfg);
}

int main(int argc, char** argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        std::cout
            << "madbackuper — бэкап и деплой сайта на удалённый сервер\n"
            << "\n"
            << "Использование: madbackuper [ключи]\n"
            << "\n"
            << "Ключи:\n"
            << "  --target-server=nginx|apache2  целевой веб-сервер\n"
            << "  --remote-host=HOST             удалённый SSH-хост\n"
            << "  --remote-user=USER            удалённый пользователь\n"
            << "  --remote-pass=PASS            SSH-пароль\n"
            << "  --local-site-dir=DIR          локальный каталог сайта\n"
            << "  --remote-site-dir=DIR         удалённый каталог сайта\n"
            << "  --remote-backup-base=DIR      удалённый каталог бэкапов\n"
            << "  --server-name=NAME            имя сайта (server_name)\n"
            << "  --php-version=VER             версия PHP\n"
            << "  --db-user=USER                пользователь БД\n"
            << "  --db-pass=PASS                пароль БД\n"
            << "  --db-name=NAME                имя БД\n"
            << "  --proxy-target=HOST           адрес бекенда для переключателя\n"
            << "  --local-http-port=PORT        локальный HTTP-порт (не 80/443)\n"
            << "  --skip-tar                    пропустить создание архива\n"
            << "  --skip-sql                    пропустить дамп БД\n"
            << "  --skip-upload                 пропустить загрузку на удалённый хост\n"
            << "  --daemon                      запустить в режиме демона\n"
            << "  --daemon-install              установить демон (systemd)\n"
            << "  --daemon-uninstall            удалить демон\n"
            << "  --at=HH:MM                    время запуска для демона\n"
            << "  --proxy                       запустить оркестратор на proxy-хосте\n"
            << "  --proxy-install               установить оркестратор на proxy-хост (SSH)\n"
            << "  --proxy-uninstall             удалить оркестратор с proxy-хоста\n"
            << "  --switch=SITE:BACKEND         ручной форс бэкенда (имя зеркала | main | auto)\n"
            << "  --status                      показать статус\n"
            << "  --site=NAME                   выбрать сайт\n"
            << "  --mirror=NAME                 выбрать зеркало\n"
            << "  --help, -h                    показать эту справку\n"
            << "  --version                     показать версию\n";
        return 0;
    }
    if (has_flag(argc, argv, "--version")) {
        std::cout << "madbackuper 1.0.0\n";
        return 0;
    }

    const bool want_daemon      = has_flag(argc, argv, "--daemon") || has_flag(argc, argv, "--daemon-mode");
    const bool want_install     = has_flag(argc, argv, "--daemon-install");
    const bool want_uninstall   = has_flag(argc, argv, "--daemon-uninstall");

    // Спец-режимы (install/uninstall) — обрабатываем раньше всего и всегда выходим.
    if (want_install || want_uninstall) {
        mad::Config cfg;
        load_or_create_config(cfg);
        mad::apply_cli_kv(argc, argv, cfg);  // позволяем переопределить удалённый хост/порты и т.п.

        std::string verr;
        if (!mad::validate(cfg, verr)) {
            std::cerr << "❌ Ошибка параметров: " << verr << "\n";
            return 1;
        }

        if (want_install)   return mad::daemon_install(cfg, ""/*auto-detect self via readlink*/);
        if (want_uninstall) return mad::daemon_uninstall(cfg);
    }

    // Режим демона — наш современный код
    if (want_daemon) {
        mad::Config cfg;
        load_or_create_config(cfg);
        mad::apply_cli_kv(argc, argv, cfg);

        std::string verr;
        if (!mad::validate(cfg, verr)) {
            std::cerr << "❌ Ошибка параметров: " << verr << "\n";
            return 1;
        }
        return mad::run_daemon_loop(cfg);
    }

    // ── Новые режимы: proxy / switch / status ──
    const bool want_proxy           = has_flag(argc, argv, "--proxy");
    const bool want_proxy_install   = has_flag(argc, argv, "--proxy-install");
    const bool want_proxy_uninstall = has_flag(argc, argv, "--proxy-uninstall");
    const bool want_status          = has_flag(argc, argv, "--status");
    const std::string switch_val    = arg_value(argc, argv, "switch");

    if (want_proxy || want_proxy_install || want_proxy_uninstall || want_status || !switch_val.empty()) {
        mad::Config cfg;
        load_or_create_config(cfg);
        mad::apply_cli_kv(argc, argv, cfg);

        std::string verr;
        if (!mad::validate(cfg, verr)) {
            std::cerr << "❌ Ошибка параметров: " << verr << "\n";
            return 1;
        }

        if (want_proxy)            return mad::proxy_run(cfg);
        if (want_proxy_install)    return mad::proxy_install(cfg);
        if (want_proxy_uninstall)  return mad::proxy_uninstall(cfg);
        if (want_status)           return mad::status_report(cfg);
        if (!switch_val.empty()) {
            const auto colon = switch_val.find(':');
            if (colon == std::string::npos) {
                std::cerr << "❌ --switch=SITE:BACKEND (BACKEND = имя зеркала | main | auto)\n";
                return 1;
            }
            return mad::proxy_switch(cfg, switch_val.substr(0, colon), switch_val.substr(colon + 1));
        }
    }

    // Выбор сайта/зеркала для legacy-режима (бэкап/деплой)
    const std::string site_sel   = arg_value(argc, argv, "site");
    const std::string mirror_sel = arg_value(argc, argv, "mirror");

    // Обычный однократный запуск — отдаём управление legacy-реализации
    return mad_legacy_main(argc, argv, site_sel, mirror_sel);
}
