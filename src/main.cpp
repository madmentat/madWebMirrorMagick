#include <filesystem>
#include <string>
#include <iostream>

#include "mad/core.hpp"
#include "mad/net.hpp"
#include "mad/deploy.hpp"
#include "mad/daemon.hpp"

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

    // Обычный однократный запуск — отдаём управление legacy-реализации
    return mad_legacy_main(argc, argv);
}
