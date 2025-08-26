// src/modules/net.cpp
#include <string>
#include <cstdint>
#include <iostream>
#include <fstream>     // std::ifstream
#include <sstream>     // std::ostringstream
#include <iomanip>     // std::setprecision, std::fixed
#include <memory>      // std::unique_ptr
#include <chrono>      // ETA расчёт
#include <fcntl.h>     // O_WRONLY | O_CREAT | O_TRUNC

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include "mad/net.hpp"
#include "mad/core.hpp"   // human_size, fs-алиас, clock_

// Удобные алиасы/using для реализации
using mad::clock_;
namespace fs = mad::fs;

namespace { // внутренние помощники только для этого файла
    inline void print_progress_line(const std::string& label,
                                    uint64_t sent, uint64_t total, double mbps)
    {
        double ratio = total ? static_cast<double>(sent) / static_cast<double>(total) : 0.0;
        int width = 28;
        int fill = static_cast<int>(ratio * width);
        uint64_t remain = total > sent ? (total - sent) : 0;
        double eta = (mbps > 0) ? (remain/1024.0/1024.0)/mbps : 0.0;

        std::cout << "\r" << label << " [";
        for (int i = 0; i < width; ++i) std::cout << (i < fill ? "█" : " ");
        std::cout << "] " << std::fixed << std::setprecision(1)
                  << (ratio * 100.0) << "%  "
                  << std::setprecision(2) << mbps << " MB/s  "
                  << mad::human_size(sent) << "/" << mad::human_size(total)
                  << "  ETA " << std::setprecision(0) << eta << "s" << std::flush;
    }
} // anonymous

namespace mad {

// --------------------------- SSH helpers ---------------------------
int ssh_exec(ssh_session session, const std::string& cmd, bool print_out) {
    ssh_channel ch = ssh_channel_new(session);
    if (!ch) { std::cerr << "❌ ssh_channel_new: " << ssh_get_error(session) << "\n"; return -1; }
    if (ssh_channel_open_session(ch) != SSH_OK) {
        std::cerr << "❌ ssh_channel_open_session: " << ssh_get_error(session) << "\n";
        ssh_channel_free(ch); return -1;
    }
    if (ssh_channel_request_exec(ch, cmd.c_str()) != SSH_OK) {
        std::cerr << "❌ ssh_channel_request_exec: " << ssh_get_error(session) << "\n";
        ssh_channel_close(ch); ssh_channel_free(ch); return -1;
    }
    char buf[4096]; int n;
    if (print_out) {
        while ((n = ssh_channel_read(ch, buf, sizeof(buf), 0)) > 0) std::cout.write(buf, n);
        while ((n = ssh_channel_read(ch, buf, sizeof(buf), 1)) > 0) std::cerr.write(buf, n);
    } else {
        while ((n = ssh_channel_read(ch, buf, sizeof(buf), 0)) > 0) {}
        while ((n = ssh_channel_read(ch, buf, sizeof(buf), 1)) > 0) {}
    }
    ssh_channel_send_eof(ch);
    int exit_status = ssh_channel_get_exit_status(ch);
    ssh_channel_close(ch); ssh_channel_free(ch);
    return exit_status;
}

int ssh_exec_capture(ssh_session session, const std::string& cmd, std::string& out, std::string* err) {
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

// --------------------------- SFTP helpers ---------------------------
const char* sftp_errname(int code) {
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

int sftp_mkdirs(ssh_session session, sftp_session sftp, const std::string& path, mode_t mode) {
    if (path.empty()) return SSH_OK;
    std::string cur;
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
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

int sftp_upload_file_progress(ssh_session session, sftp_session sftp,
                              const std::string& local, const std::string& remote,
                              const std::string& label, int* out_err, mode_t mode)
{
    if (out_err) *out_err = SSH_FX_OK;

    std::ifstream in(local, std::ios::binary);
    if (!in) { std::cerr << "❌ Не открыть локальный файл: " << local << "\n"; return -1; }

    uint64_t total = 0;
    try { total = fs::file_size(local); } catch (...) { total = 0; }

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

    uint64_t sent = 0;
    auto t0 = clock_::now();
    auto last = t0;

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
                double mbps = secs > 0 ? (sent/1024.0/1024.0)/secs : 0.0;
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

// Резервная передача через обычный SSH-канал (cat > file.part), тоже с прогрессом
int ssh_stream_upload(ssh_session session,
                      const std::string& local, const std::string& remote,
                      const std::string& label)
{
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
    if (!in) {
        std::cerr << "❌ Не открыть локальный файл: " << local << "\n";
        ssh_channel_close(ch); ssh_channel_free(ch); return -1;
    }

    uint64_t total = 0; try { total = fs::file_size(local); } catch (...) { total = 0; }

    const size_t BUFSZ = 128 * 1024;
    std::unique_ptr<char[]> buf(new char[BUFSZ]);

    uint64_t sent = 0;
    auto t0 = clock_::now();
    auto last = t0;

    while (in) {
        in.read(buf.get(), BUFSZ);
        std::streamsize n = in.gcount();
        if (n <= 0) break;

        const char* p = buf.get();
        while (n > 0) {
            int wr = ssh_channel_write(ch, p, static_cast<uint32_t>(n));
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
                double mbps = secs > 0 ? (sent/1024.0/1024.0)/secs : 0.0;
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

    // атомарное переименование .part -> финальное имя
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

// Экранирование строки для безопасного использования в shell-команде
std::string sh_escape_single(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";  // закрыть кавычку, вставить \', открыть снова
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
}

} // namespace mad
