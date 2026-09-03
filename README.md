# madWebMirrorMagick

`madbackuper` — C++17-утилита резервного копирования и аварийного переключения веб-сервера.

Она делает локальный архив сайта и `mysqldump`, передаёт их на резерв по SSH/SFTP, разворачивает резервную копию и поддерживает watchdog для nginx failover.

## Что изменено после рефакторинга

Legacy unity-build удалён. Исходники разделены на нормальные модули:

- `core` — конфигурация и общие функции;
- `net` — SSH/SFTP;
- `backup` — архивирование, дамп, передача и планировщик;
- `deploy` — staging/rollback развёртывания;
- `daemon` — health-check, failover и systemd installation.

## Сборка

Ubuntu/Debian:

```bash
sudo apt install g++ make libssh-dev
make -j2
```

Отладочная сборка с ASan/UBSan:

```bash
make debug
```

## Первый запуск

```bash
./madbackuper backup
```

Если конфигурации ещё нет, будет создан `~/.config/madbackuper.conf` с правами `0600`. При запуске из systemd обычно используется `/etc/madbackuper.conf`.

## SSH

Программа сначала пробует SSH-ключ и только затем пароль из конфига. Проверка host key обязательна: неизвестный или изменившийся ключ приводит к отказу подключения. Ключ сервера следует проверить и заранее добавить в `known_hosts` штатными средствами OpenSSH.

## Права

Обычный backup/deploy больше не создаёт `sudoers`, не монтирует файловые системы и не редактирует `/etc/fstab`.

Удалённый пользователь должен заранее иметь права на:

- `remote_backup_base`;
- `remote_site_dir` и его родительский каталог (для staging/rollback);
- указанную БД через `db_user`.

База и её пользователь создаются один раз администратором. Программа больше не выполняет `CREATE USER`/`GRANT ALL` от MySQL root при каждом deploy.

Для ручного failover от непривилегированного SSH-пользователя нужен только узкий sudo-доступ к root-owned switch script, например двум конкретным командам `... local` и `... remote`. Универсальные `sudo tee`, `mv`, `mount`, `tar` и подобные права не нужны.

## Режимы

```bash
./madbackuper backup
./madbackuper --daemon
./madbackuper monitor
./madbackuper install
./madbackuper uninstall
```

`--daemon` сохранён для совместимости и снова означает ежедневный backup по `schedule_hhmm`. Команда `install` ставит отдельный systemd timer для backup и watchdog на резервной машине.

## Надёжность deploy

Сайт сначала распаковывается в staging-каталог. Старый webroot сохраняется как `.old`; при ошибке импорта БД webroot откатывается. Ошибка удалённого deploy теперь возвращается вызывающему процессу как ненулевой exit code.

Архивы, SQL и временные DB credentials создаются с закрытыми правами; DB credentials удаляются после deploy.
