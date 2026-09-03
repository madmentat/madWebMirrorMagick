# madWebMirrorMagick

`madWebMirrorMagick` — утилита для зеркалирования веб-сервера: резервное копирование файлов и БД, передача на резервный узел, health-check и failover.

Текущая ветка `develop` содержит новую модульную архитектуру и встроенную административную панель **madUI**.

## Быстрый старт

На Ubuntu/Debian-подобном сервере:

```bash
./install.sh
```

Если файл был получен без executable-bit:

```bash
bash install.sh
```

Установщик:

1. проверяет sudo-сеанс;
2. при необходимости устанавливает `build-essential` и `libssh-dev`;
3. собирает проект;
4. устанавливает бинарник как `/usr/local/bin/madwebmirror`;
5. оставляет совместимый alias `/usr/local/bin/madbackuper`;
6. сразу запускает **madUI**.

После запуска терминал печатает один или несколько адресов вида:

```text
http://192.168.88.198:8790/
```

Откройте адрес в браузере. Вкладка получит одноразовый код, например:

```text
F7K2-W9Q4
```

В том же sudo-терминале появится запрос с IP и User-Agent браузера. Только после подтверждения `y` этой вкладке выдаётся временная `HttpOnly` session-cookie.

**Пароль sudo/root сама программа не получает и не сохраняет.** Он используется только механизмом `sudo` для запуска административного процесса. После `Ctrl+C` madUI завершается, а все browser-session исчезают из памяти.

### Ручной запуск madUI

```bash
sudo madwebmirror ui
```

По умолчанию madUI слушает IPv4 `0.0.0.0:8790`, потому что сервер обычно настраивается из браузера другой машины в локальной сети.

Можно ограничить доступ loopback-интерфейсом:

```bash
sudo madwebmirror ui --ui-bind=127.0.0.1 --ui-port=8790
```

Для удалённого сервера этот вариант удобно использовать вместе с SSH tunnel.

## Что уже умеет madUI

- terminal trust handshake для новых браузеров;
- отдельный on-demand административный режим — UI не обязан постоянно торчать наружу;
- показывает текущую primary/mirror архитектуру;
- редактирует ключевые параметры сайта, SSH, БД, backup schedule и health-check;
- сохраняет конфигурацию атомарно с правами `0600`;
- не показывает и не изменяет sudo/root password;
- API закрыт до терминального подтверждения браузера;
- SameSite session-cookie, same-origin проверка POST и базовые security headers.

Интерфейс пока является первой рабочей оболочкой. Следующие логичные модули UI: обнаружение сервисов на узле, SSH enrollment удалённого узла, тест соединения, управление модулями, история backup/failover, лог событий и мастер установки systemd-компонентов.

## CLI

```text
madbackuper backup
madbackuper --daemon
madbackuper monitor
sudo madbackuper ui
madbackuper install
madbackuper uninstall
```

`madbackuper` сохранён как совместимое имя. Новое установленное имя — `madwebmirror`.

## Сборка вручную

```bash
sudo apt-get install build-essential libssh-dev
make
```

Debug-сборка:

```bash
make debug
```

Она включает AddressSanitizer и UndefinedBehaviorSanitizer.

## Безопасность

SSH-соединение требует известный host key (`known_hosts`). Сначала используется SSH key, пароль допускается как fallback для совместимости.

Обычная работа программы не должна зависеть от хранения root/sudo password. Привилегированные операции постепенно выносятся в отдельный минимальный helper/bootstrap-контур.

Конфигурация хранится с правами `0600`. Старые поля `remote_pass`/`remote_sudo_pass` пока остаются ради совместимости со старой конфигурацией, но madUI намеренно не выставляет sudo password как постоянную настройку.

## Ветки

- `snapshot` — зафиксированное состояние старой рабочей версии до рефакторинга;
- `release` — прежняя release-версия;
- `develop` — текущая разработка, включая madUI.
