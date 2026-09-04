# madWebMirrorMagick

**madWebMirrorMagick** — утилита для резервного копирования, зеркалирования и аварийного переключения веб-серверов.

Она создаёт копии файлов и базы данных, передаёт их на резервный узел, проверяет доступность основного сайта и управляет переключением между рабочими backend-серверами. Для узлов за NAT поддерживаются два взаимозаменяемых SSH-маршрута через Proxy A и Proxy B.

Проект рассчитан прежде всего на Debian и Ubuntu, nginx или Apache, MySQL/MariaDB и systemd.

## Возможности

- резервное копирование каталога сайта и базы данных;
- передача архивов через SSH/SFTP;
- развёртывание копии на удалённом сервере;
- ежедневный backup по расписанию;
- health-check и автоматическое переключение frontend-маршрута;
- защита от ложных переключений при кратковременных сбоях;
- автономный failover-agent на резервном узле, не зависящий от madUI;
- сохранение состояния маршрута и его повторная сверка после перезапуска;
- прямой SSH-доступ или подключение через два альтернативных Proxy;
- постоянные LocalForward и RemoteForward SSH-туннели;
- автоматическое переключение туннеля между Proxy A и Proxy B;
- SSH enrollment с отдельными ключами Ed25519;
- строгая проверка ключей узлов через общий `known_hosts`;
- встроенная административная панель **madUI**;
- работа основных служб от непривилегированного пользователя `madbackup`;
- ограниченный root-helper для необходимых системных операций;
- установка бинарников, systemd-служб и каталогов одним скриптом.

## Схема работы

```text
                         Controller / madUI
                           /             \
                          /               \
                     Proxy A            Proxy B
                        \                 /
                         \               /
                          main <-----> mirror
```

Proxy A и Proxy B — альтернативные маршруты, а не последовательная цепочка. Если один Proxy недоступен, контроллер может связаться с приватным узлом через второй.

```text
direct  controller ---------------------> target
jump    controller -> Proxy A ----------> target
                    -> Proxy B ----------> target (fallback)
auto    direct, затем Proxy A, затем Proxy B
```

Подробное описание ролей узлов, привилегий и отказоустойчивости находится в [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Требования

- Linux с systemd;
- компилятор C++17 и GNU Make;
- libssh и OpenSSH client;
- `sudo`;
- `tar`, `curl` и клиент MySQL/MariaDB для рабочих операций;
- nginx или Apache на обслуживаемых узлах.

## Установка

На Debian или Ubuntu:

```bash
git clone https://github.com/madmentat/madWebMirrorMagick.git
cd madWebMirrorMagick
sudo ./install.sh
```

Если исполняемый бит скрипта потерялся:

```bash
sudo bash install.sh
```

Установщик проверяет зависимости, собирает проект, создаёт пользователя `madbackup`, устанавливает `madwebmirror`, совместимое имя `madbackuper`, ограниченный helper и автономный `madweb-agent`, создаёт служебные каталоги, устанавливает tunnel supervisor и запускает первоначальную настройку через madUI.

Ручная сборка:

```bash
sudo apt-get install build-essential libssh-dev openssh-client sudo
make -j"$(nproc)"
```

Отладочная сборка с AddressSanitizer и UndefinedBehaviorSanitizer:

```bash
make debug
```

## Первоначальная настройка

```bash
sudo madwebmirror ui
```

По умолчанию панель слушает IPv4-адрес `0.0.0.0:8790`. Ограничить доступ локальным компьютером можно так:

```bash
sudo madwebmirror ui --ui-bind=127.0.0.1 --ui-port=8790
```

При открытии панели браузер получает одноразовый код. Новый сеанс начинает работать только после подтверждения в том же терминале, из которого запущена madUI. Пароль `sudo` или `root` в браузер не передаётся и приложением не сохраняется.

Через madUI настраиваются узлы, тип веб-сервера, каталоги, база данных, расписание, health-check, SSH transport, оба Proxy и постоянные туннели.

Поле **Watchdog health URL** задаёт адрес основного backend с точки зрения резервного узла. Если оно пусто, используется `http://<proxy_target>:<local_http_port>/`.

## Автономный failover-agent

Команда `madwebmirror install` передаёт на резервный узел отдельный бинарник `madweb-agent`, конфигурацию без SSH- и DB-паролей и systemd-службу `madbackuper-watchdog.service`.

Агент самостоятельно:

- проверяет доступность основного backend;
- применяет `local` после заданного числа ошибок;
- возвращает `remote` после устойчивого восстановления;
- сохраняет режим и время последнего переключения в `/var/lib/madwebmirror/failover.state`;
- после каждого запуска повторно применяет выбранный маршрут, поэтому сохранённое состояние не расходится с реальной конфигурацией nginx;
- блокирует запуск второго экземпляра watchdog.

Текущее состояние на резервном сервере можно посмотреть так:

```bash
sudo /usr/local/libexec/madweb-agent status
```

После отказа контроллера уже установленный агент продолжает обслуживать failover. Новые backup при этом пока не создаются: распределённый планировщик резервного копирования остаётся следующим этапом.

## SSH enrollment

Для первого подключения и установки ключей:

```bash
sudo madwebmirror enroll
```

Пароль и подтверждение fingerprint вводятся непосредственно в терминале. Runtime-подключения используют ключи и строгую проверку `known_hosts`.

По умолчанию ключи находятся в `/var/lib/madwebmirror/ssh/`, а общий trust store — в `/var/lib/madwebmirror/.ssh/known_hosts`.

Пример конфигурации узла за двумя Proxy:

```ini
ssh_transport=jump
ssh_identity_file=/var/lib/madwebmirror/ssh/target-private-web

ssh_jump_primary=madbackup@proxy-a.example.net:22
ssh_jump_primary_identity_file=/var/lib/madwebmirror/ssh/proxy-a

ssh_jump_fallback=madbackup@proxy-b.example.net:22
ssh_jump_fallback_identity_file=/var/lib/madwebmirror/ssh/proxy-b
```

## SSH Tunnel Manager

Политика туннелей хранится в `/etc/madwebmirror/tunnels.conf`.

```text
tunnel=ID|ROUTE|DIRECTION|BIND_HOST|BIND_PORT|TARGET_HOST|TARGET_PORT|ENABLED
```

Пример failover-группы LocalForward через два Proxy:

```ini
tunnel=web-admin|primary|local|127.0.0.1|18080|192.168.1.20|80|true
tunnel=web-admin|fallback|local|127.0.0.1|18080|192.168.1.20|80|true
```

Одинаковый `ID` объединяет маршруты в одну failover-группу. Одновременно работает один SSH-процесс. Если активный маршрут завершается, supervisor запускает второй; если недоступны оба, попытки повторяются.

Ручной запуск:

```bash
madwebmirror tunnels
```

При штатной установке supervisor работает через `madwebmirror-tunnels.service` от пользователя `madbackup`.

## Командная строка

```text
madwebmirror backup       один backup и deploy
madwebmirror --daemon     ежедневный backup по расписанию
madwebmirror monitor      health-check и failover
madwebmirror ui           административная веб-панель
madwebmirror enroll       первоначальная установка SSH-ключей
madwebmirror tunnels      supervisor SSH-туннелей
madwebmirror install      установка backup-служб и watchdog
madwebmirror uninstall    удаление установленных служб
```

Полная справка:

```bash
madwebmirror --help
```

## Привилегии и безопасность

Основные процессы работают от отдельного пользователя `madbackup`. Операции с системными каталогами и конфигурацией веб-сервера выполняет root-owned программа `madweb-helper` с закрытым набором команд и проверкой аргументов.

Helper не предоставляет команд вида `exec`, `shell` или записи произвольного текста в системные конфигурационные файлы. Перед применением конфигурации nginx или Apache выполняется её штатная проверка. Runtime SSH-подключения требуют известный host key.

Старые параметры `remote_pass` и `remote_sudo_pass` пока поддерживаются для совместимости. Новые установки должны использовать SSH-ключи. Окончательное удаление парольного режима остаётся одной из задач перед стабильным выпуском.

## Текущее состояние

Ветка `release` содержит текущую публикуемую версию. Разработка продолжается в `develop`. Исходная монолитная реализация сохранена в `legacy`.

Проект собирается в GitHub Actions и проходит CLI-тесты, включая смену маршрута, восстановление состояния и повторную сверку автономного watchdog. Перед применением на рабочем сервере всё ещё рекомендуется проверить установку на тестовых узлах: автоматические end-to-end тесты реального backup/deploy и настоящей конфигурации nginx/Apache пока не реализованы.

## Ветки

- `release` — текущая публикуемая версия;
- `develop` — основная ветка разработки;
- `legacy` — исходная рабочая монолитная версия;
- `snapshot` — прежний технический снимок, совпадающий с началом `legacy`.

## Лицензия

Проект распространяется на условиях [The Unlicense](LICENSE).
