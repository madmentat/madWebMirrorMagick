#pragma once
#include <string>
#include "mad/core.hpp"
#include "mad/net.hpp"

namespace mad {

// Установка/удаление демона (локально + подготовка вочдога на 202)
int daemon_install(const Config& cfg, const std::string& self_path);
int daemon_uninstall(const Config& cfg);

// Запускает фоновой цикл демона: health-check локального сайта,
// при падении — переключение фронта на remote (202), при восстановлении — обратно.
int run_daemon_loop(const Config& cfg);

// Проверка локального сайта: true = OK, false = down.
// По умолчанию пингуем http://127.0.0.1:<local_http_port> через curl -fsSL -m 5
bool check_local_site_ok(const Config& cfg);

// Переключение nginx на 202 в режим локального фронта (local) — когда 198 недоступен
int remote_switch_to_local_nginx(const Config& cfg);

// Переключение nginx на 202 в режим удалённого фронта (remote) — когда 198 снова жив
int remote_switch_to_remote_nginx(const Config& cfg);

// Установка/проверка systemd-юнитов на 198 и 202 (скелет, пока не вызываем каждый тик)
int ensure_watchdog_units(const Config& cfg);

} // namespace mad
