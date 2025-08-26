#!/usr/bin/env bash
set -euo pipefail

MONO="${1:-madbackuper.cpp}"
if [[ ! -f "$MONO" ]]; then
  echo "❌ Не найден $MONO (можно передать путь первым аргументом)"; exit 1
fi

echo "→ Готовлю каркас..."
mkdir -p include/mad src/modules src/legacy

# --- Makefile ---
cat > Makefile <<'MK'
CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Iinclude
LDFLAGS  := -lssh
TARGET   := madbackuper

SRC_MAIN := src/main.cpp
SRC_MODS := src/modules/core.cpp src/modules/net.cpp src/modules/deploy.cpp

OBJ := $(SRC_MAIN:.cpp=.o) $(SRC_MODS:.cpp=.o)

all: $(TARGET)
$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
.PHONY: all clean
MK

# --- Заголовки (минимум, сгруппировано по смыслу) ---
cat > include/mad/core.hpp <<'HPP'
#pragma once
#include <string>
#include <cstdint>
#include <filesystem>
#include <chrono>

namespace mad {
namespace fs = std::filesystem;
using clock_ = std::chrono::steady_clock;

struct Config; // будет жить в core.cpp, здесь только forward

// utils
std::string base64_encode(const std::string& in);
std::string trim(const std::string& s);
std::string today();
std::string human_size(uint64_t bytes);
uint64_t dir_size_bytes(const std::filesystem::path& root);
bool has_command(const char* name);
int  run_local(const std::string& cmd, bool echo = true);
int  run_with_spinner(const std::string& cmd, const std::string& label);

// config IO & validation
extern const char* CFG_PATH_PRIMARY;
extern const char* CFG_PATH_FALLBACK;
extern const char* DEFAULT_TARGET;
extern const int   DEFAULT_SSH_PORT;
extern const int   DEFAULT_LOCAL_HTTP_PORT;
extern const int   DEFAULT_LOCAL_HTTPS_PORT;
extern const bool  DEFAULT_SWITCH_TO_LOCAL;
extern const std::string DEFAULT_PHP_VERSION;

struct Config; // full def в core.cpp
void write_default_config(const std::string& path);
void load_kv_file(const std::string& path, Config& cfg);
void apply_cli_kv(int argc, char** argv, Config& cfg);
bool validate(const Config& c, std::string& err);
} // namespace mad
HPP

cat > include/mad/net.hpp <<'HPP'
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
HPP

cat > include/mad/deploy.hpp <<'HPP'
#pragma once
#include <string>
namespace mad {
struct Config;
std::string build_nginx_deploy_cmd(const Config& C,
                                   const std::string& remote_tar,
                                   const std::string& remote_sql,
                                   const std::string& remote_day);
std::string build_apache_deploy_cmd(const Config& C,
                                    const std::string& remote_tar,
                                    const std::string& remote_sql,
                                    const std::string& remote_day);
} // namespace mad
HPP

# --- main.cpp с unity-мостом ---
cat > src/main.cpp <<'CPP'
#include "mad/core.hpp"
#include "mad/net.hpp"
#include "mad/deploy.hpp"

// unity-включение legacy с переименованием main
#define main mad_legacy_main
#include "legacy/madbackuper.cpp"
#undef main

int main(int argc, char** argv) {
    return mad_legacy_main(argc, argv);
}
CPP

# --- Заготовки модулей ---
cat > src/modules/core.cpp <<'CPP'
#include <string>
#include <cstdint>
#include <filesystem>
#include <chrono>
#include <iostream>
#include <vector>
#include "mad/core.hpp"

namespace mad {
// сюда скрипт вставит: Константы, base64, Конфиг, Вспомогалки, Конфиг I/O, Проверка параметров
} // namespace mad
CPP

cat > src/modules/net.cpp <<'CPP'
#include <string>
#include <cstdint>
#include <iostream>
#include "mad/net.hpp"
#include "mad/core.hpp"

namespace mad {
// сюда скрипт вставит: SSH helpers, SFTP helpers, Префикс sudo, Проверка места и bootstrap
} // namespace mad
CPP

cat > src/modules/deploy.cpp <<'CPP'
#include <string>
#include <sstream>
#include "mad/deploy.hpp"
#include "mad/core.hpp"
#include "mad/net.hpp"

namespace mad {
// сюда скрипт вставит: Развёртывание (NGINX/APACHE)
} // namespace mad
CPP

# --- Перенос исходника в legacy (с бэкапом) ---
if [[ -f src/legacy/madbackuper.cpp ]]; then
  cp -f src/legacy/madbackuper.cpp src/legacy/madbackuper.cpp.bak.$(date +%s)
fi
cp -f "$MONO" src/legacy/madbackuper.cpp

# --- вспомогалка: извлечение секции по заголовку-комменту ---
extract_section() {
  local title="$1" out_cpp="$2"
  # Берём текст между нужным заголовком и следующим '---------------------------'
  awk -v t="$title" '
    BEGIN{grab=0}
    /\/\/ ---------------------------/ {
      if (grab==1) { exit } # дошли до следующего раздела
    }
    $0 ~ t { grab=1; next }
    grab==1 { print }
  ' src/legacy/madbackuper.cpp \
  | sed -E 's/^[[:space:]]*static[[:space:]]+/\t/g' \
  >> "$out_cpp"
}

# --- комментирование секции в legacy (чтобы не было дублей при линковке) ---
comment_out_section_in_legacy() {
  local title="$1"
  awk -v t="$title" '
    BEGIN{grab=0}
    {
      if ($0 ~ t) { 
        if (grab==0) { print "// --- ⬇ перенесено в модули: " t; print "#if 0"; grab=1; next }
      }
      if (grab==1 && /\/\/ ---------------------------/) { print "#endif"; grab=2 }
      if (!(grab==1)) print
    }
  ' src/legacy/madbackuper.cpp > src/legacy/madbackuper.cpp.__tmp__
  mv src/legacy/madbackuper.cpp.__tmp__ src/legacy/madbackuper.cpp
}

echo "→ Переношу секции по смысловым группам..."

# CORE
for tag in \
  "// --------------------------- Константы ---------------------------" \
  "// ===== base64 encoder (локальный, без зависимостей) =====" \
  "// --------------------------- Конфиг ---------------------------" \
  "// --------------------------- Вспомогалки ---------------------------" \
  "// --------------------------- Конфиг I/O ---------------------------" \
  "// --------------------------- Проверка параметров ---------------------------"
do
  extract_section "$tag" "src/modules/core.cpp"
  comment_out_section_in_legacy "$tag"
done

# NET
for tag in \
  "// --------------------------- SSH helpers ---------------------------" \
  "// --------------------------- SFTP helpers ---------------------------" \
  "// --------------------------- Префикс sudo и пр. ---------------------------" \
  "// --------------------------- Проверка места и bootstrap ---------------------------"
do
  extract_section "$tag" "src/modules/net.cpp"
  comment_out_section_in_legacy "$tag"
done

# DEPLOY
for tag in \
  "// --------------------------- Развёртывание командой (NGINX) ---------------------------" \
  "// --------------------------- Развёртывание командой (APACHE) ---------------------------"
do
  extract_section "$tag" "src/modules/deploy.cpp"
  comment_out_section_in_legacy "$tag"
done

# Доп. чистка: обрамляем вставленный код в namespace mad (вставлено уже каркасом),
# а ключевое слово 'static' на верхнем уровне сняли sed'ом выше.
# Конец — напоминание
echo "✓ Готово."
echo "Теперь положи этот проект рядом с исходником и запускай:  make"
echo "Бинарь: ./madbackuper"
