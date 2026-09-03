#include "mad/backup.hpp"

#include <libssh/sftp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "mad/deploy.hpp"
#include "mad/net.hpp"

namespace mad {
namespace {

std::atomic<bool> stop_requested{false};

void on_signal(int) {
    stop_requested = true;
}

std::string mysql_option_quote(const std::string& value) {
    std::string out{"\""};
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

fs::path cache_dir_for_today() {
    const char* home = std::getenv("HOME");
    fs::path base = home && *home ? fs::path(home) / ".cache" / "madbackuper"
                                  : fs::temp_directory_path() / ("madbackuper-" + std::to_string(::geteuid()));
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) throw std::runtime_error("Не удалось создать cache-каталог: " + base.string());
    ::chmod(base.c_str(), 0700);

    fs::path day = base / today();
    fs::create_directories(day, ec);
    if (ec) throw std::runtime_error("Не удалось создать рабочий каталог: " + day.string());
    ::chmod(day.c_str(), 0700);
    return day;
}

bool upload_with_fallback(ssh_session session, sftp_session sftp,
                          const std::string& local, const std::string& remote,
                          const std::string& label, mode_t mode = 0600) {
    int sftp_error = SSH_FX_OK;
    if (sftp_upload_file_progress(session, sftp, local, remote, label, &sftp_error, mode) == 0) return true;
    std::cerr << "⚠️ SFTP: " << sftp_errname(sftp_error) << ", пробую SSH stream\n";
    return ssh_stream_upload(session, local, remote, label + "(ssh)") == 0;
}

std::time_t next_local_time(int hh, int mm) {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    local.tm_hour = hh;
    local.tm_min = mm;
    local.tm_sec = 0;
    std::time_t result = std::mktime(&local);
    if (result <= now) {
        ++local.tm_mday;
        result = std::mktime(&local);
    }
    return result;
}

void sleep_until(std::time_t target) {
    while (!stop_requested) {
        const std::time_t now = std::time(nullptr);
        if (now >= target) return;
        const auto remaining = target - now;
        std::this_thread::sleep_for(std::chrono::seconds(remaining > 30 ? 30 : remaining));
    }
}

} // namespace

int run_backup_once(const Config& cfg) {
    std::cout << "📁 Сайт: " << cfg.local_site_dir << '\n'
              << "🛢️ БД: " << cfg.db_name << '\n'
              << "🌐 Резерв: " << cfg.remote_user << '@' << cfg.remote_host << ':' << cfg.remote_site_dir << '\n';

    if (!fs::exists(cfg.local_site_dir) || !fs::is_directory(cfg.local_site_dir)) {
        std::cerr << "❌ Нет каталога сайта: " << cfg.local_site_dir << '\n';
        return 1;
    }
    if (!has_command("tar") || !has_command("gzip") || !has_command("mysqldump")) {
        std::cerr << "❌ Нужны tar, gzip и mysqldump\n";
        return 1;
    }

    fs::path work;
    try {
        work = cache_dir_for_today();
    } catch (const std::exception& e) {
        std::cerr << "❌ " << e.what() << '\n';
        return 1;
    }

    const std::string date = today();
    const fs::path tar_path = work / ("site_" + date + ".tar.gz");
    const fs::path sql_path = work / ("db_" + date + ".sql");
    const fs::path cnf_path = work / ".db.cnf";

    {
        std::ofstream cnf(cnf_path, std::ios::trunc);
        if (!cnf) {
            std::cerr << "❌ Не удалось создать " << cnf_path << '\n';
            return 1;
        }
        cnf << "[client]\nuser=" << mysql_option_quote(cfg.db_user) << '\n'
            << "password=" << mysql_option_quote(cfg.db_pass) << '\n';
        cnf.close();
        ::chmod(cnf_path.c_str(), 0600);
    }

    if (!(cfg.skip_tar && fs::exists(tar_path) && fs::file_size(tar_path) > 0)) {
        const auto size = dir_size_bytes(cfg.local_site_dir);
        std::cout << "📦 Архивация сайта (~" << human_size(size) << ")...\n";
        const std::string cmd = "umask 077; tar -C " + shell_quote(cfg.local_site_dir) +
                                " -cf - . | gzip > " + shell_quote(tar_path.string());
        if (run_local(cmd, false) != 0) {
            fs::remove(cnf_path);
            return 1;
        }
        ::chmod(tar_path.c_str(), 0600);
    } else {
        std::cout << "⏭ Использую готовый архив " << tar_path << '\n';
    }

    if (!(cfg.skip_sql && fs::exists(sql_path) && fs::file_size(sql_path) > 0)) {
        std::cout << "🛢️ Дамп БД...\n";
        const std::string cmd = "umask 077; mysqldump --defaults-extra-file=" + shell_quote(cnf_path.string()) +
                                " --single-transaction --quick --routines --triggers --events --no-tablespaces " +
                                shell_quote(cfg.db_name) + " > " + shell_quote(sql_path.string());
        if (run_local(cmd, false) != 0) {
            fs::remove(cnf_path);
            return 1;
        }
        ::chmod(sql_path.c_str(), 0600);
    } else {
        std::cout << "⏭ Использую готовый дамп " << sql_path << '\n';
    }

    std::string ssh_error;
    ssh_session session = ssh_connect_authenticated(cfg, ssh_error);
    if (!session) {
        std::cerr << "❌ " << ssh_error << '\n';
        fs::remove(cnf_path);
        return 1;
    }
    std::cout << "✅ SSH: " << peer_ip(session) << '\n';

    if (prepare_remote_directories(session, cfg) != 0) {
        ssh_disconnect_and_free(session);
        fs::remove(cnf_path);
        return 1;
    }

    std::uint64_t need = 0;
    std::error_code ec;
    need += fs::file_size(tar_path, ec); ec.clear();
    need += fs::file_size(sql_path, ec); ec.clear();
    if (ensure_remote_space(session, cfg, need) != 0) {
        ssh_disconnect_and_free(session);
        fs::remove(cnf_path);
        return 1;
    }

    sftp_session sftp = sftp_new(session);
    if (!sftp || sftp_init(sftp) != SSH_OK) {
        std::cerr << "❌ Не удалось открыть SFTP\n";
        if (sftp) sftp_free(sftp);
        ssh_disconnect_and_free(session);
        fs::remove(cnf_path);
        return 1;
    }

    const std::string remote_day = cfg.remote_backup_base + "/" + date;
    const std::string remote_tar = remote_day + "/site_" + date + ".tar.gz";
    const std::string remote_sql = remote_day + "/db_" + date + ".sql";
    const std::string remote_cnf = remote_day + "/.db.cnf." + timestamp();

    if (sftp_mkdirs(session, sftp, remote_day, 0700) != SSH_OK) {
        sftp_free(sftp);
        ssh_disconnect_and_free(session);
        fs::remove(cnf_path);
        return 1;
    }

    bool upload_tar = true;
    bool upload_sql = true;
    if (cfg.skip_upload) {
        upload_tar = !remote_file_nonzero(sftp, remote_tar);
        upload_sql = !remote_file_nonzero(sftp, remote_sql);
    }

    bool ok = true;
    if (upload_tar) ok = upload_with_fallback(session, sftp, tar_path.string(), remote_tar, "Архив") && ok;
    else std::cout << "⏭ Архив уже есть на резерве\n";
    if (upload_sql) ok = upload_with_fallback(session, sftp, sql_path.string(), remote_sql, "Дамп") && ok;
    else std::cout << "⏭ Дамп уже есть на резерве\n";
    ok = upload_with_fallback(session, sftp, cnf_path.string(), remote_cnf, "DB config", 0600) && ok;

    fs::remove(cnf_path);
    if (!ok) {
        ssh_exec(session, "rm -f -- " + shell_quote(remote_cnf), false);
        sftp_free(sftp);
        ssh_disconnect_and_free(session);
        return 1;
    }

    const std::string deploy_cmd = cfg.target_server == "nginx"
        ? build_nginx_deploy_cmd(cfg, remote_tar, remote_sql, remote_cnf, remote_day)
        : build_apache_deploy_cmd(cfg, remote_tar, remote_sql, remote_cnf, remote_day);

    std::cout << "🧩 Развёртывание на резерве...\n";
    const int deploy_rc = ssh_exec(session, deploy_cmd, true);
    if (deploy_rc != 0) {
        std::cerr << "❌ Развёртывание завершилось с ошибкой " << deploy_rc << '\n';
        ssh_exec(session, "rm -f -- " + shell_quote(remote_cnf), false);
        sftp_free(sftp);
        ssh_disconnect_and_free(session);
        return 1;
    }

    sftp_free(sftp);
    ssh_disconnect_and_free(session);
    std::cout << "🎉 Бэкап и развёртывание завершены успешно.\n";
    return 0;
}

int run_backup_daemon(const Config& cfg) {
    int hh = 0;
    int mm = 0;
    if (!parse_hhmm(cfg.schedule_hhmm, hh, mm)) return 1;

    stop_requested = false;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "🕒 Backup daemon: ежедневный запуск в " << cfg.schedule_hhmm << '\n';
    while (!stop_requested) {
        const std::time_t next = next_local_time(hh, mm);
        char buf[64]{};
        std::tm local{};
        localtime_r(&next, &local);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local);
        std::cout << "⏳ Следующий запуск: " << buf << '\n';
        sleep_until(next);
        if (stop_requested) break;
        if (run_backup_once(cfg) != 0) std::cerr << "⚠️ Плановый бэкап завершился ошибкой\n";
        for (int i = 0; i < 60 && !stop_requested; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}

} // namespace mad
