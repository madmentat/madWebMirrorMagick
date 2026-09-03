#include "mad/net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

namespace mad {
namespace {

using steady_clock = std::chrono::steady_clock;

void print_progress_line(const std::string& label, std::uint64_t sent,
                         std::uint64_t total, double mbps) {
    const double ratio = total ? static_cast<double>(sent) / static_cast<double>(total) : 0.0;
    constexpr int width = 28;
    const int fill = static_cast<int>(ratio * width);
    const std::uint64_t remain = total > sent ? total - sent : 0;
    const double eta = mbps > 0.0 ? (static_cast<double>(remain) / 1024.0 / 1024.0) / mbps : 0.0;

    std::cout << '\r' << label << " [";
    for (int i = 0; i < width; ++i) std::cout << (i < fill ? "█" : " ");
    std::cout << "] " << std::fixed << std::setprecision(1) << ratio * 100.0 << "%  "
              << std::setprecision(2) << mbps << " MB/s  " << human_size(sent) << '/'
              << human_size(total) << "  ETA " << std::setprecision(0) << eta << 's' << std::flush;
}

bool verify_known_host(ssh_session session, std::string& err) {
    const int state = ssh_session_is_known_server(session);
    if (state == SSH_KNOWN_HOSTS_OK) return true;

    switch (state) {
        case SSH_KNOWN_HOSTS_CHANGED:
            err = "SSH host key изменился. Подключение остановлено.";
            break;
        case SSH_KNOWN_HOSTS_OTHER:
            err = "Для SSH-хоста найден ключ другого типа. Подключение остановлено.";
            break;
        case SSH_KNOWN_HOSTS_NOT_FOUND:
        case SSH_KNOWN_HOSTS_UNKNOWN:
            err = "SSH host key отсутствует в known_hosts. Сначала проверьте ключ сервера и добавьте его в known_hosts.";
            break;
        case SSH_KNOWN_HOSTS_ERROR:
        default:
            err = std::string("Не удалось проверить SSH host key: ") + ssh_get_error(session);
            break;
    }
    return false;
}

} // namespace

ssh_session ssh_connect_authenticated(const Config& cfg, std::string& err) {
    err.clear();
    ssh_session session = ssh_new();
    if (!session) {
        err = "ssh_new() вернул nullptr";
        return nullptr;
    }

    int timeout = 30;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(session, SSH_OPTIONS_HOST, cfg.remote_host.c_str());
    ssh_options_set(session, SSH_OPTIONS_USER, cfg.remote_user.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &cfg.ssh_port);

    if (ssh_connect(session) != SSH_OK) {
        err = std::string("ssh_connect: ") + ssh_get_error(session);
        ssh_free(session);
        return nullptr;
    }

    if (!verify_known_host(session, err)) {
        ssh_disconnect(session);
        ssh_free(session);
        return nullptr;
    }

    int auth = ssh_userauth_publickey_auto(session, nullptr, nullptr);
    if (auth != SSH_AUTH_SUCCESS && !cfg.remote_pass.empty()) {
        auth = ssh_userauth_password(session, nullptr, cfg.remote_pass.c_str());
    }
    if (auth != SSH_AUTH_SUCCESS) {
        err = std::string("SSH-аутентификация не удалась: ") + ssh_get_error(session);
        ssh_disconnect(session);
        ssh_free(session);
        return nullptr;
    }
    return session;
}

void ssh_disconnect_and_free(ssh_session session) {
    if (!session) return;
    ssh_disconnect(session);
    ssh_free(session);
}

int ssh_exec(ssh_session session, const std::string& cmd, bool print_out) {
    ssh_channel channel = ssh_channel_new(session);
    if (!channel) return -1;
    if (ssh_channel_open_session(channel) != SSH_OK) {
        ssh_channel_free(channel);
        return -1;
    }
    if (ssh_channel_request_exec(channel, cmd.c_str()) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return -1;
    }

    char buf[4096];
    int n = 0;
    while ((n = ssh_channel_read(channel, buf, sizeof(buf), 0)) > 0) {
        if (print_out) std::cout.write(buf, n);
    }
    while ((n = ssh_channel_read(channel, buf, sizeof(buf), 1)) > 0) {
        if (print_out) std::cerr.write(buf, n);
    }

    ssh_channel_send_eof(channel);
    const int status = ssh_channel_get_exit_status(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return status;
}

int ssh_exec_capture(ssh_session session, const std::string& cmd,
                     std::string& out, std::string* err) {
    out.clear();
    if (err) err->clear();

    ssh_channel channel = ssh_channel_new(session);
    if (!channel) return -1;
    if (ssh_channel_open_session(channel) != SSH_OK) {
        ssh_channel_free(channel);
        return -1;
    }
    if (ssh_channel_request_exec(channel, cmd.c_str()) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return -1;
    }

    char buf[4096];
    int n = 0;
    while ((n = ssh_channel_read(channel, buf, sizeof(buf), 0)) > 0) out.append(buf, n);
    if (err) {
        while ((n = ssh_channel_read(channel, buf, sizeof(buf), 1)) > 0) err->append(buf, n);
    } else {
        while ((n = ssh_channel_read(channel, buf, sizeof(buf), 1)) > 0) {}
    }

    ssh_channel_send_eof(channel);
    const int status = ssh_channel_get_exit_status(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return status;
}

std::string peer_ip(ssh_session session) {
    const int sock = ssh_get_fd(session);
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "unknown";

    char ip[INET6_ADDRSTRLEN]{};
    if (addr.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    } else if (addr.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&addr);
        inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
    } else {
        return "unknown";
    }
    return ip;
}

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

int sftp_mkdirs(ssh_session session, sftp_session sftp,
                 const std::string& path, mode_t mode) {
    if (path.empty()) return SSH_OK;

    std::string current;
    std::size_t start = 0;
    if (path.front() == '/') {
        current = "/";
        start = 1;
    }

    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string::npos ? path.size() : slash;
        const auto part = path.substr(start, end - start);
        if (!part.empty()) {
            if (current.size() > 1 && current.back() != '/') current.push_back('/');
            current += part;
            if (sftp_mkdir(sftp, current.c_str(), mode) != SSH_OK) {
                const int error = sftp_get_error(sftp);
                if (error != SSH_FX_FILE_ALREADY_EXISTS) {
                    std::cerr << "❌ sftp_mkdir(" << current << "): " << sftp_errname(error)
                              << " — " << ssh_get_error(session) << '\n';
                    return SSH_ERROR;
                }
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return SSH_OK;
}

int sftp_upload_file_progress(ssh_session session, sftp_session sftp,
                              const std::string& local, const std::string& remote,
                              const std::string& label, int* out_err, mode_t mode) {
    if (out_err) *out_err = SSH_FX_OK;

    std::ifstream in(local, std::ios::binary);
    if (!in) return -1;

    std::uint64_t total = 0;
    std::error_code ec;
    total = fs::file_size(local, ec);
    if (ec) total = 0;

    sftp_file file = sftp_open(sftp, remote.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (!file) {
        const int error = sftp_get_error(sftp);
        if (out_err) *out_err = error;
        std::cerr << "❌ sftp_open(" << remote << "): " << sftp_errname(error)
                  << " — " << ssh_get_error(session) << '\n';
        return -1;
    }

    constexpr std::size_t buf_size = 128 * 1024;
    std::unique_ptr<char[]> buf(new char[buf_size]);
    std::uint64_t sent = 0;
    const auto started = steady_clock::now();
    auto last = started;

    while (in) {
        in.read(buf.get(), static_cast<std::streamsize>(buf_size));
        std::streamsize left = in.gcount();
        char* ptr = buf.get();
        while (left > 0) {
            const ssize_t written = sftp_write(file, ptr, static_cast<std::size_t>(left));
            if (written <= 0) {
                const int error = sftp_get_error(sftp);
                if (out_err) *out_err = error;
                sftp_close(file);
                return -1;
            }
            ptr += written;
            left -= written;
            sent += static_cast<std::uint64_t>(written);

            const auto now = steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= 250 || sent == total) {
                const double secs = std::chrono::duration<double>(now - started).count();
                const double mbps = secs > 0.0 ? static_cast<double>(sent) / 1024.0 / 1024.0 / secs : 0.0;
                print_progress_line(label, sent, total, mbps);
                last = now;
            }
        }
    }

    sftp_close(file);
    std::cout << '\r' << label << " [████████████████████████████] 100.0%  ✓  "
              << human_size(sent) << "                    \n";
    return 0;
}

int ssh_stream_upload(ssh_session session, const std::string& local,
                      const std::string& remote, const std::string& label) {
    const std::string part = remote + ".part";
    ssh_channel channel = ssh_channel_new(session);
    if (!channel) return -1;
    if (ssh_channel_open_session(channel) != SSH_OK) {
        ssh_channel_free(channel);
        return -1;
    }

    const std::string command = "umask 077; cat > " + shell_quote(part);
    if (ssh_channel_request_exec(channel, command.c_str()) != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return -1;
    }

    std::ifstream in(local, std::ios::binary);
    if (!in) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return -1;
    }

    std::uint64_t total = 0;
    std::error_code ec;
    total = fs::file_size(local, ec);
    if (ec) total = 0;

    constexpr std::size_t buf_size = 128 * 1024;
    std::unique_ptr<char[]> buf(new char[buf_size]);
    std::uint64_t sent = 0;
    const auto started = steady_clock::now();
    auto last = started;

    while (in) {
        in.read(buf.get(), static_cast<std::streamsize>(buf_size));
        std::streamsize left = in.gcount();
        const char* ptr = buf.get();
        while (left > 0) {
            const int written = ssh_channel_write(channel, ptr, static_cast<std::uint32_t>(left));
            if (written <= 0) {
                ssh_channel_send_eof(channel);
                ssh_channel_close(channel);
                ssh_channel_free(channel);
                return -1;
            }
            ptr += written;
            left -= written;
            sent += static_cast<std::uint64_t>(written);

            const auto now = steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= 250 || sent == total) {
                const double secs = std::chrono::duration<double>(now - started).count();
                const double mbps = secs > 0.0 ? static_cast<double>(sent) / 1024.0 / 1024.0 / secs : 0.0;
                print_progress_line(label, sent, total, mbps);
                last = now;
            }
        }
    }

    ssh_channel_send_eof(channel);
    const int status = ssh_channel_get_exit_status(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    if (status != 0) return -1;

    const std::string move_cmd = "mv -f -- " + shell_quote(part) + " " + shell_quote(remote);
    return ssh_exec(session, move_cmd, false);
}

std::uint64_t remote_bytes_avail(ssh_session session, const std::string& path) {
    std::string out;
    std::string err;
    const std::string command = "df -B1 --output=avail -- " + shell_quote(path) +
                                " 2>/dev/null | tail -n1 | tr -d '[:space:]'";
    if (ssh_exec_capture(session, command, out, &err) != 0) return 0;
    try {
        return static_cast<std::uint64_t>(std::stoull(trim(out)));
    } catch (...) {
        return 0;
    }
}

int prepare_remote_directories(ssh_session session, const Config& cfg) {
    const std::string command = "umask 077; mkdir -p -- " + shell_quote(cfg.remote_backup_base) +
                                " " + shell_quote(cfg.remote_site_dir);
    if (ssh_exec(session, command, false) != 0) {
        std::cerr << "❌ Удалённый пользователь не может создать рабочие каталоги. "
                     "Права следует подготовить один раз администратором; программа больше не выдаёт себе sudo.\n";
        return 1;
    }
    return 0;
}

int ensure_remote_space(ssh_session session, const Config& cfg, std::uint64_t need_bytes) {
    const auto available = remote_bytes_avail(session, cfg.remote_backup_base);
    const std::uint64_t headroom = std::max<std::uint64_t>(need_bytes / 10, 512ULL * 1024ULL * 1024ULL);
    const std::uint64_t required = need_bytes + headroom;
    std::cout << "💽 На резерве требуется ~" << human_size(required)
              << ", доступно ~" << human_size(available) << '\n';
    if (available < required) {
        std::cerr << "❌ Недостаточно места на резервном сервере. Автоматический mount/fstab отключён.\n";
        return 1;
    }
    return 0;
}

bool remote_file_nonzero(sftp_session sftp, const std::string& path) {
    sftp_attributes attr = sftp_stat(sftp, path.c_str());
    if (!attr) return false;
    const bool ok = attr->type == SSH_FILEXFER_TYPE_REGULAR && attr->size > 0;
    sftp_attributes_free(attr);
    return ok;
}

} // namespace mad
