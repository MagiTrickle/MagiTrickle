<p align="center">
  <img src="https://gitlab.com/magitrickle/magitrickle/-/raw/develop/img/logo256.png" alt="MagiTrickle logo"/>
</p>

MagiTrickle
=======

## Назначение

MagiTrickle - Маршрутизация трафика на основе DNS запросов для роутеров Keenetic (под управлением [Entware](https://github.com/The-BB/Entware-Keenetic)).

*(Продукт в данный момент находится в состоянии разработки)*

Данное программное обеспечение реализует маршрутизацию трафика на основе проксирования через себя DNS запросов. Можно указать список доменных имён, которые нужно маршрутизировать на тот, или иной интерфейс, вместо бесконечного накопления IP адресов.

## Особенности, в сравнении с другим ПО

1. Не требует отключения встроенного в Keenetic DNS сервера - всё работает методом перенаправления портов.
2. Работает с любыми туннелями, которые умеют поднимать UNIX интерфейс.
3. Несколько типов правил - domain, namespace, wildcard и regex.
4. Не тянет за собой огромное количество сторонних пакетов. Вся конфигурация находится в одном месте (в одном файле).
5. Возможность создавать несколько групп на разные сети.
6. Моментальное бесшовное включение/выключение сервиса.

## Установка

1. Устанавливаем пакет:
```bash
opkg install magitrickle_<version>_<arch>.ipk
```
2. Копируем конфиг __**(⚠️ только для уверенных пользователей)**__:
```bash
cp /opt/var/lib/magitrickle/config.yaml.example /opt/var/lib/magitrickle/config.yaml
```
3. Настраиваем конфиг __**(⚠️ только для уверенных пользователей)**__:
```yaml
configVersion: 0.1.2
app:                          # Настройки программы
  httpWeb:
    enabled: true             # Включение HTTP сервера
    host:
      address: '[::]'         # Адрес, который будет слушать программа для приёма HTTP запросов
      port: 8080              # Порт
    skin: default             # Оболочка (по пути /opt/usr/bin/share/magitrickle/skins)
  dnsProxy:
    host:
      address: '[::]'         # Адрес, который будет слушать программа для приёма DNS запросов
      port: 3553              # Порт
    upstream:
      address: 127.0.0.1      # Адрес, используемый для отправки DNS запросов
      port: 53                # Порт
    disableRemap53: false     # Флаг отключения перепривязки 53 порта
    disableFakePTR: false     # Флаг отключения подделки PTR записи (без неё есть проблемы, может быть будет исправлено в будущем)
    disableDropAAAA: false    # Флаг отключения откидывания AAAA записей
  netfilter:
    iptables:
      chainPrefix: MT_        # Префикс для названий цепочек IPTables
    ipset:
      tablePrefix: mt_        # Префикс для названий таблиц IPSet
      additionalTTL: 3600     # Дополнительный TTL (если от DNS пришел TTL 300, то к этому числу прибавится указанный TTL)
    disableIPv4: false        # Отключить управление IPv4
    disableIPv6: false        # Отключить управление IPv6
    startMarkTableIndex: 1234 # Стартовый индекс для метки и таблицы маршрутизации пакетов
  link:                       # Список адресов где будет подменяться DNS
    - br0
    - br1
  showAllInterfaces: false    # Показывать все интерфейсы (не только с флагом PointToPoint)
  logLevel: info              # Уровень логов (trace, debug, info, warn, error)
```
4. Запускаем сервис:
```bash
/opt/etc/init.d/S99magitrickle start
```
5. Добавляем адреса в панели сервиса по адресу `<IP_Роутера>:8080`

## Отладка
Если вам нужна отладка, то останавливаем сервис и запускаем "демона" руками:
```bash
/opt/etc/init.d/S99magitrickle stop
magitrickled
```

## Описание типов правил

*   _**Namespace**_ - Именное пространство.

    Определяет сам домен и все его поддомены.

    Так например при записи "`example.com`" правила будут обрабатываться как:

    ```
    ✅ example.com
    ✅ sub.example.com
    ✅ sub.sub.example.com
    ❌ anotherexample.com
    ❌ example.net
    ```

*   _**Wildcard**_ - Шаблон с `*` и `?`.

    Позволяет использовать `*` и `?` для гибкого соответствия доменам.

    Так например при записи "`*.example.com`" правила будут обрабатываться как:

    ```
    ❌ example.com
    ✅ sub.example.com
    ✅ sub.sub.example.com
    ❌ anotherexample.com
    ❌ example.net
    ```

*   _**Domain**_ - Точный домен.

    Охватывает только указанный домен, без поддоменов.

    Так например при записи "`example.com`" правила будут обрабатываться как:

    ```
    ✅ example.com
    ❌ sub.example.com
    ❌ sub.sub.example.com
    ❌ anotherexample.com
    ❌ example.net
    ```

*   _**RegEx**_ - для продвинутых пользователей. Если это определение для тебя неизвестно - лучше не лезть!

## Поддержка

* [Форум на Keenetic Community](https://forum.keenetic.ru/topic/20125-magitrickle)
* [Канал Telegram](https://t.me/MagiTrickle)
* [Чат Telegram](https://t.me/MagiTrickleChat)
* [Поддержать проект (Оставить чаевые)](https://hipolink.net/magitrickle)