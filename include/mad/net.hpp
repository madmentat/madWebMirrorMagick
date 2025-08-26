#pragma once
#include <string>
#include <cstdint>
#include <libssh/libssh.h>
#include <libssh/sftp.h>

namespace mad {
struct Config;

// ssh
int  ssh_exec(ssh_session session, const std::string& cmd, bool print_out = true);
int  ssh_exec_capture(ssh_session session, const std::string& cmd, std::string& out, std::string* err=nullptr);
std::string peer_ip(ssh_session s);

// sftp / transfer
const char* sftp_errname(int code);
int sftp_mkdirs(ssh_session session, sftp_session sftp, const std::string& path, mode_t mode = 0755);
int sftp_upload_file_progress(ssh_session session, sftp_session sftp,
                              const std::string& local, const std::string& remote,
                              const std::string& label, int* out_err=nullptr, mode_t mode = 0644);
int ssh_stream_upload(ssh_session session, const std::string& local, const std::string& remote, const std::string& label);

// sudo & remote prep
std::string sh_escape_single(const std::string& s);
std::string sudo_prefix(ssh_session session, const Config& C);
uint64_t remote_bytes_avail(ssh_session session, const std::string& path);
int ensure_space_and_bind_mount(ssh_session session, const Config& C, uint64_t need_bytes);
int bootstrap_remote(ssh_session session, const Config& C);
} // namespace mad
