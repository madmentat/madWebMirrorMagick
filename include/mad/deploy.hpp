#pragma once

#include <string>

#include "mad/core.hpp"

namespace mad {

std::string build_nginx_deploy_cmd(const Config& cfg,
                                   const std::string& remote_tar,
                                   const std::string& remote_sql,
                                   const std::string& remote_db_cnf,
                                   const std::string& remote_day);

std::string build_apache_deploy_cmd(const Config& cfg,
                                    const std::string& remote_tar,
                                    const std::string& remote_sql,
                                    const std::string& remote_db_cnf,
                                    const std::string& remote_day);

std::string resolved_switch_script(const Config& cfg);
std::string build_nginx_switch_to_local_cmd(const Config& cfg);
std::string build_nginx_switch_to_remote_cmd(const Config& cfg);

} // namespace mad
