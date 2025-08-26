#pragma once
#include <string>
#include "mad/core.hpp"

namespace mad {

// Основной деплой (как и было)
std::string build_nginx_deploy_cmd(const Config& C,
                                   const std::string& remote_tar,
                                   const std::string& remote_sql,
                                   const std::string& remote_day);

std::string build_apache_deploy_cmd(const Config& C,
                                    const std::string& remote_tar,
                                    const std::string& remote_sql,
                                    const std::string& remote_day);

// Новые команды для «переключения фронта» на удалённом nginx
std::string build_nginx_switch_to_local_cmd (const Config& C);
std::string build_nginx_switch_to_remote_cmd(const Config& C);

} // namespace mad
