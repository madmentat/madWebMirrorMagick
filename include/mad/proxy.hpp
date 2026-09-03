#pragma once
#include "mad/core.hpp"

namespace mad {

// Режим --proxy: бесконечный цикл оркестратора, крутится НА proxy-хосте (root).
// Для каждого сайта проверяет бэкенды [mirror1..N, main] и маршрутизирует
// на первого живого, переписывая конфиг nginx/apache и перезагружая сервер.
int proxy_run(const Config& cfg);

// Установка оркестратора на proxy-хост (SSH + SFTP + systemd-юнит).
int proxy_install(const Config& cfg);

// Удаление оркестратора с proxy-хоста.
int proxy_uninstall(const Config& cfg);

// Ручной форс бэкенда: --switch=SITE:BACKEND (BACKEND = имя зеркала | main | auto).
int proxy_switch(const Config& cfg, const std::string& site_name, const std::string& backend);

} // namespace mad
