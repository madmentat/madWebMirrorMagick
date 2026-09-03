# madWebMirrorMagick

`madWebMirrorMagick` — утилита для зеркалирования и отказоустойчивости веб-серверов: резервное копирование файлов и БД, передача на резервные узлы, health-check, failover и управление двумя взаимозаменяемыми Proxy.

Текущая ветка `develop` содержит модульную архитектуру и встроенную административную панель **madUI**.

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
2. устанавливает `build-essential`, `libssh-dev`, `openssh-client` и `sudo`, если они нужны;
3. собирает проект;
4. создаёт непривилегированного системного пользователя `madbackup`;
5. устанавливает `/usr/local/bin/madwebmirror` и совместимый alias `madbackuper`;
6. устанавливает root-owned `/usr/local/libexec/madweb-helper` и узкое sudoers-правило только на него;
7. создаёт `/srv/madwebmirror`, `/var/lib/madwebmirror/ssh` и общий SSH trust store;
8. устанавливает `madwebmirror-tunnels.service` под `User=madbackup`;
9. сразу запускает **madUI**.

После запуска терминал печатает адрес вида:

```text
http://192.168.88.198:8790/
```

Откройте его в браузере. Вкладка получает одноразовый код, например `F7K2-W9Q4`. В том же sudo-терминале появится запрос с IP и User-Agent браузера. Только после подтверждения `y` вкладке выдаётся временная `HttpOnly` session-cookie.

**Пароль sudo/root сама программа не получает и не сохраняет.** После `Ctrl+C` madUI завершается, browser-session исчезают из памяти.

## Два Proxy и узлы за NAT

Proxy — capability узла, а не отдельный жёсткий тип машины. Proxy A и Proxy B являются альтернативными bastion-маршрутами:

```text
                 Controller / madUI
                   /             \
                  /               \
             Proxy A            Proxy B
               |                   |
               +--------+----------+
                        |
                  private web node
                    192.168.1.20
```

Для managed target доступны transport modes:

```text
direct  controller ----------------------> target
jump    controller -> Proxy A ----------> target
                    -> Proxy B ----------> target (fallback)
auto    direct, затем Proxy A, затем Proxy B
```

Proxy A и Proxy B не образуют последовательную цепочку A → B. Если один Proxy недоступен, управление приватным backend должно оставаться доступным через второй.

## SSH enrollment

madUI и CLI умеют создавать раздельные Ed25519 identities для:

- конечного target node;
- Proxy A;
- Proxy B.

CLI:

```bash
sudo madwebmirror enroll
```

При первом подключении `ssh-copy-id` может попросить пароль и подтверждение fingerprint. Ввод происходит **не в браузере**, а непосредственно в административном терминале. Пароль не возвращается приложению и не записывается в конфиг.

Ключи по умолчанию находятся под:

```text
/var/lib/madwebmirror/ssh/
```

Общий проверенный `known_hosts`:

```text
/var/lib/madwebmirror/.ssh/known_hosts
```

Один и тот же trust store используется tunnel supervisor, OpenSSH jump transport и libssh/SFTP-клиентом.

Пример transport-конфигурации:

```ini
ssh_transport=jump
ssh_identity_file=/var/lib/madwebmirror/ssh/target-private-web

ssh_jump_primary=madbackup@proxy-a.example.net:22
ssh_jump_primary_identity_file=/var/lib/madwebmirror/ssh/proxy-a

ssh_jump_fallback=madbackup@proxy-b.example.net:22
ssh_jump_fallback_identity_file=/var/lib/madwebmirror/ssh/proxy-b
```

## SSH Tunnel Manager

Tunnel policy хранится в:

```text
/etc/madwebmirror/tunnels.conf
```

Формат строки:

```text
tunnel=ID|ROUTE|DIRECTION|BIND_HOST|BIND_PORT|TARGET_HOST|TARGET_PORT|ENABLED
```

Например одна логическая LocalForward-группа через оба Proxy:

```text
tunnel=web-admin|primary|local|127.0.0.1|18080|192.168.1.20|80|true
tunnel=web-admin|fallback|local|127.0.0.1|18080|192.168.1.20|80|true
```

Одинаковый `ID` означает **failover-группу**. Одновременно активен только один SSH-процесс, поэтому два Proxy не дерутся за один local bind. Если активный маршрут завершается, supervisor пробует второй Proxy; если недоступны оба — повторяет попытки.

Поддерживаются:

- `local` (`ssh -L`);
- `remote` (`ssh -R`);
- независимые группы туннелей;
- отдельный identity key для каждого Proxy;
- strict host-key checking;
- `ExitOnForwardFailure` и keepalive;
- автоматический A/B failover.

Ручной запуск:

```bash
madwebmirror tunnels
```

При обычной установке используется systemd unit:

```text
madwebmirror-tunnels.service
```

Он работает как `madbackup`, а не root. Tunnel policy остаётся `root:madbackup 0640`: сервис может читать её, но не переписывать.

Из madUI можно сохранить policy, запустить, перезапустить и остановить supervisor. Эти операции проходят через фиксированные verbs `madweb-helper`, а не через произвольный `sudo systemctl ...`.

## Привилегии

Обычные процессы работают без root. Однократный bootstrap устанавливает helper и правило:

```text
madbackup ALL=(root) NOPASSWD: /usr/local/libexec/madweb-helper *
```

Это не даёт произвольный root shell: helper содержит закрытый набор проверяемых операций. Среди них подготовка site storage, безопасная генерация nginx/apache route и управление только конкретным tunnel service.

Копии сайтов создаются автоматически:

```text
/srv/madwebmirror/sites/<site-id>/
    releases/
    shared/
```

Ручная предварительная подготовка web-папок не требуется после bootstrap.

## madUI

Ручной запуск:

```bash
sudo madwebmirror ui
```

По умолчанию madUI слушает IPv4 `0.0.0.0:8790`. Можно ограничить loopback:

```bash
sudo madwebmirror ui --ui-bind=127.0.0.1 --ui-port=8790
```

В текущем madUI есть:

- terminal trust handshake;
- карточки Proxy A / Proxy B;
- `direct / jump / auto` transport;
- пути identity keys;
- запуск SSH enrollment;
- редактор LocalForward/RemoteForward;
- создание A/B failover-пары одним действием;
- состояние tunnel systemd-service;
- `start / restart / stop` tunnel supervisor;
- параметры сайта, БД, backup schedule и health-check;
- same-origin POST, SameSite cookie и security headers.

madUI разрешает сохранять первый этап конфигурации до заполнения всех site/DB полей. Полная operational validation выполняется перед backup/deploy.

## CLI

```text
madwebmirror backup
madwebmirror --daemon
madwebmirror monitor
sudo madwebmirror ui
sudo madwebmirror enroll
madwebmirror tunnels
madwebmirror install
madwebmirror uninstall
```

`madbackuper` остаётся совместимым alias.

## Сборка вручную

```bash
sudo apt-get install build-essential libssh-dev openssh-client sudo
make
```

Debug:

```bash
make debug
```

Включены AddressSanitizer и UndefinedBehaviorSanitizer.

## Безопасность и текущие ограничения

SSH требует заранее подтверждённый host key. Jump-host password authentication не используется в runtime; первый пароль нужен только для enrollment.

Старые поля `remote_pass` и `remote_sudo_pass` пока остаются в `Config` ради совместимости со старой конфигурацией. madUI их не выставляет как постоянные настройки; их удаление/перенос в отдельный bootstrap-secret store остаётся следующим этапом hardening.

Текущий tunnel failover является стабильным: после перехода на Proxy B он не переключается обратно на A, пока B остаётся жив. Это специально избегает лишнего flapping; более умный controlled failback можно добавить позже.

## Ветки

- `snapshot` — зафиксированная старая рабочая версия;
- `release` — прежняя release-ветка;
- `develop` — текущая разработка.
