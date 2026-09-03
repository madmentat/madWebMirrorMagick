#pragma once

#include <cstdint>
#include <string>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include "mad/core.hpp"

namespace mad {

ssh_session ssh_connect_authenticated(const Config& cfg, std::string& err);
void ssh_disconnect_and_free(ssh_session session);

int ssh_exec(ssh_session session, const std::string& cmd, bool print_out = true);
int ssh_exec_capture(ssh_session session, const std::string& cmd, std::string& out, std::string* err = nullptr);
std::string peer_ip(ssh_session session);

const char* sftp_errname(int code);
int sftp_mkdirs(ssh_session session, sftp_session sftp, const std::string& path, mode_t mode = 0755);
int sftp_upload_file_progress(ssh_session session, sftp_session sftp,
                              const std::string& local, const std::string& remote,
                              const std::string& label, int* out_err = nullptr,
                              mode_t mode = 0600);
int ssh_stream_upload(ssh_session session, const std::string& local,
                      const std::string& remote, const std::string& label);

std::uint64_t remote_bytes_avail(ssh_session session, const std::string& path);
int prepare_remote_directories(ssh_session session, const Config& cfg);
int ensure_remote_space(ssh_session session, const Config& cfg, std::uint64_t need_bytes);
bool remote_file_nonzero(sftp_session sftp, const std::string& path);

} // namespace mad
