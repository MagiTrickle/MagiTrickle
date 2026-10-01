# Phase 5 — отчёт (netfilter & netlink)

Дата: 2026-07-23. Ветка: `claude/magitrickle-c-rewrite-phase-0-t7iwas`.
Go backend не изменён.

## Выполнено

- **iptables engine** (`src/iptables/{rule,chain_patch,chain_override,
  chain_delete,engine,executable_real}.c`, `include/magitrickle/iptables.h`)
  — порт `utils/iptables` (Go): patch/override/delete chain-семантика,
  компиляция транскрипта по приоритету, `GetCurrentRules` (парсинг
  `iptables-save`), `Commit()` через save/restore одним batch'ем.
  Executable-транспорт — реальный (`posix_spawn`, фиксированный argv, без
  shell) и fake in-memory (порт `executable-fake.go`) для тестов.
- **iptables differential** (`tests/unit/test_iptables.c`) — 16 тестов,
  98 assertions, портирующих каждый кейс `utils/iptables/iptables_test.go`
  против fake-транспорта — **все проходят**.
- **ipset via libmnl** (`src/netfilter/{ipset,ipset_nl_real}.c`,
  `include/magitrickle/ipset.h`) — NFNETLINK/`NFNL_SUBSYS_IPSET`
  create/destroy/add/del/list4/list6 с таймаутами и `Replace`;
  wire-формат (протокол зафиксирован на версии **6**, а не на текущем
  `IPSET_PROTOCOL=7` из UAPI-заголовков ядра) воспроизведён из того, что
  реально отправляет `vishvananda/netlink` (см. D-20). Fake-транспорт
  (`tests/unit/fake_ipset_nl.c`) + 12 юнит-тестов, 53 assertions.
- **ipset-to-link + port-remap + cleaner**
  (`src/netfilter/{ipset_to_link,port_remap,cleaner}.c`) — порт
  `ipset-to-link.go` (iptables filter/mangle/nat правила, ip rule, ip
  route с blackhole-фоллбэком и gateway-diffing), `port-remap.go` (DNAT
  53→proxy-порт) и `iptables-cleaner.go` (сметание чужих/старых
  MT_-цепочек и висячих `-j MT_*` правил на старте).
- **rtnetlink** (`src/netlink/rtnl.c`, `include/magitrickle/rtnl.h`) —
  ip rule add/del, ip route add/del (blackhole и через интерфейс+gateway),
  link-by-name, gateway-for-iface, аллокация свободного mark/table —
  через libmnl вместо `vishvananda/netlink`. Единственный из
  netfilter-модулей Phase 5, реально проверенный против живого ядра в
  этой песочнице (см. D-21).
- **netlink watcher** (`src/netlink/watcher.c`,
  `include/magitrickle/netlink_watcher.h`) — подписка на
  `RTMGRP_LINK|RTMGRP_IPV4_IFADDR|RTMGRP_IPV6_IFADDR`, фильтрация событий
  точно как в Go (`RTM_NEWLINK` только при `IFF_UP`, `RTM_NEWADDR` без
  `RTM_DELADDR`), интеграция в `mt_loop` через epoll fd.
- **`mt_ruleset_t`** (`src/netfilter/ruleset.c`,
  `include/magitrickle/ruleset.h`) — новый модуль, порт `rule_set.go`.
  Существует ровно та же проблема, что и `mt_ruleset_snapshot_t` (Phase 4)
  не решает: DNS-hot-path снимок явно исключает `subnet`/`subnet6`-правила
  и не хранит `interface` группы. `mt_ruleset_t` — отдельная,
  netfilter-ориентированная структура на группу: собственный `mt_ipset_t`
  (с выделенным netlink-сокетом, см. D-20) + общий `mt_ipset_to_link_t`.
  Порт `Enable/Disable/Sync/AddIPv4Subnet/AddIPv6Subnet/LinkUpHook/
  AddrChangeHook` с той же двухуровневой enabled-семантикой, что и Go:
  `mt_ruleset_enable()` всегда взводит runtime-флаг (идемпотентно, как
  CAS в Go) даже для отключённой в конфиге группы, но реальные
  ipset/ipset-to-link объекты создаются только когда `group->enable`
  истинен — то есть **`mt_ruleset_t` существует для каждой группы из
  конфига независимо от её `enable`**, ровно как `app.userRuleSets` в Go,
  чтобы отключённая группа всё равно получила `ClearIfDisabled`/`Disable`
  при следующем старте.
  `mt_ruleset_sync()` — построчный порт `sync()`: статические CIDR из
  `subnet`/`subnet6`-правил (включая "грязный хак" `0.0.0.0/0`→два `/1`,
  `::/0`→два `/1`), доменные правила — матчинг против
  `mt_cache_list_known_domains()`/`mt_cache_get_addresses()` с
  TTL-дедупликацией, диффинг против текущего состояния ipset
  (`mt_ipset_list4/6`) с той же логикой "не понижать TTL, не трогать
  permanent-записи", что и в Go. См. D-22 про сложность диффа.
- **`main.c`** — полностью переписан: порт `start.go`'s init-последовательности
  (iptables-движки для v4/v6 если не отключены → регистрация chain-patch
  для filter/FORWARD, mangle/PREROUTING, nat/PREROUTING, nat/POSTROUTING →
  `CleanIPTables` → открытие rtnl → netlink watcher → DNS-прокси → сигналы →
  port-remap на 53 (если не отключён; список локальных адресов
  сконфигурированных интерфейсов собирается через `getifaddrs`/
  `if_nametoindex`, а не через отдельный netlink-dump — см. ниже) →
  `mt_ruleset_t` на каждую группу, Enable+Sync). Phase 4 sink-заглушка
  заменена реальной: `on_match()` находит нужный `mt_ruleset_t` по
  `group_id` из `mt_match_action_t` и вызывает `mt_ruleset_add_ipv4/6`.
  Обработчики netlink watcher'а (`on_link_up`/`on_addr_change`)
  диспатчат в каждый `mt_ruleset_t`, чей `group->iface` совпадает с
  именем интерфейса события — ровно как `handleLink`/`handleAddr` в
  Go перебирают `ruleSetSnapshot()`.
  Teardown полностью централизован в `daemon_teardown()` (все
  free/disable-вызовы NULL-safe и идемпотентны), вызывается из каждой
  точки отказа при старте и при штатной остановке — заменил россыпь
  ручных cleanup-блоков Phase 4, так как ресурсов стало на порядок
  больше (loop, cache, pipeline, ipt4, ipt6, rtnl, watcher, port_remap,
  N rulesets).

## Решения (decisions.md)

- **D-19** — единый loop-поток для всей netfilter-мутации (расширение
  D-17), без внутренних локов в `mt_ipt_t`/`mt_ipset_t`/
  `mt_ipset_to_link_t`/`mt_ruleset_t`/`mt_rtnl_t`; порядок итерации
  chain/table в движке — insertion-order массивы вместо рандомизированного
  порядка Go-мапы (не наблюдаемое расхождение, задокументировано explicit).
- **D-20** — у каждой группы свой netlink-сокет для ipset (не общий
  `pkgHandle`, как в Go) — принятая неэффективность (лишние fd), не
  поведенческое расхождение; протокол ipset зафиксирован на версии 6.
- **D-21** — rtnetlink проверяется функционально на живом ядре
  (`ip rule`/`ip route show`), а не побайтовым зеркалированием
  случайных особенностей wire-формата `vishvananda/netlink`; найденный
  и исправленный баг — лишний `NLM_F_ACK` на одиночном `RTM_GETLINK`
  десинхронизировал сокет.
- **D-22** — `sync()` использует линейный скан вместо hash-map для
  diff'а множеств подсетей — O(n²), принятый tradeoff, не заявление об
  улучшении производительности.

## Проверки

- `make CFLAGS_EXTRA=-Werror` (host, все конфигурации) — 0 warnings.
- `make test` — 12 бинарников тестов (test_ipset: 12 тестов/53 assertions,
  test_iptables: 16 тестов/98 assertions — новые в этой фазе, плюс все
  предыдущие фазы без регрессий) — все Pass.
- `make sanitize` — ASan+UBSan по всем юнит-тестам — 0 находок.
- `make static_analysis` — clang-tidy (весь `LIB_SRCS`+`MAIN_SRC`+
  тулы+test-support) + cppcheck — **0 warnings** после исправления:
  двух `bugprone-casting-through-void` (сериализация `sockaddr_in(6)` в
  `collect_link_addrs()` — заменено на `memcpy` вместо каста через
  `const void *`) и двух `bugprone-branch-clone` (идентичное тело двух
  веток `if`/`else if` в `ruleset.c`'s diff-функциях — схлопнуто в одно
  булево выражение).
- **Полная сборка демона (`magitrickled-c`) под ASan+UBSan**
  (`make BUILD=sanitize ... build/sanitize/magitrickled-c`) прогнана
  живьём в этой песочнице (см. ниже) — 0 находок на всех сценариях.
- **Функциональная проверка на реальном ядре** (в этой песочнице
  `ip_set` недоступен, как и в Phase 0, но `iproute2`/iptables-nft
  работают):
  1. Отключённая группа + `disableRemap53: false` + `link: [lo]` —
     демон стартует, слушает DNS, поднимает реальные iptables-правила
     DNAT (`nat`/`DNSOR`: `-d 127.0.0.1/32 -p tcp/udp --dport 53 -j
     DNAT --to-destination :5354`), корректно снимает их по `SIGTERM`
     (`iptables-save -t nat` после остановки — пусто).
  2. Отключённая группа + доменное правило + фейковый upstream (Python,
     возвращает A-запись `10.1.2.3`) — реальный DNS-запрос через прокси
     получает корректный ответ, pipeline обрабатывает A-запись
     (`upstream response: id=... answers=1`), `on_match`→
     `find_ruleset`→`mt_ruleset_add_ipv4` вызывается и корректно
     no-op'ает (группа отключена в конфиге) — без падений, без ASan/UBSan
     находок.
  3. **Включённая** группа с `subnet`-правилом — демон доходит до
     `mt_ruleset_enable()`, реальный libmnl-запрос `IPSET_CMD_DESTROY`
     улетает в ядро и получает `EINVAL` (подтверждено `strace`: сообщение
     `nlmsg_type=0x603` = NFNETLINK subsys 6 (ipset) msg 3 (destroy)) —
     то самое отсутствие модуля `ip_set`, найденное ещё в Phase 0.
     Демон корректно трактует это как фатальную ошибку старта (ровно как
     сделал бы Go: `if err := group.Enable(); err != nil { return ... }`),
     `daemon_teardown()` откатывает уже поднятое iptables-состояние
     (`nat`/`DNSOR` в этом прогоне не создавался, т.к. remap53 был
     отключён) — чистое состояние после выхода с кодом 1.
  4. Симметричный `mt_netfilter_clean_iptables()` (startup cleaner)
     отработал без ошибок против реального `iptables-nft` бэкенда во
     всех прогонах выше (нет унаследованных MT_-цепочек в свежей
     песочнице — сметать было нечего, но код прошёл весь путь).
- **netns/veth интеграционный тест с реальным ipset**: не выполнен —
  `ip_set` модуль ядра недоступен в этой песочнице (см. Phase 0), как
  и `dummy`-netdevice модуль (использовался бы для изолированного
  netns-теста линков) — **отложено на on-device верификацию** (Entware
  `_kn` / OpenWrt цель), как и предполагал план Phase 5's exit criterion
  "on-device smoke".

## Нерешённое / перенесённое

- Настоящий netns/veth-тест с реальным `ip_set` — заблокирован
  отсутствием модуля ядра в песочнице; перенесено на on-device smoke
  (Entware `_kn` + OpenWrt цель), как и было заложено в exit criterion
  Phase 5.
- `mt_ruleset_sync()` использует O(n²) линейный скан вместо hash-map
  (D-22) — не бенчмаркано на больших конфигурациях; пересмотреть, если
  Phase 7 (подписки, потенциально тысячи правил в одной группе) покажет
  это узким местом.
- HTTP/Unix API (Phase 6) ещё не существует — нет способа
  программно менять группы/правила во время работы демона, поэтому
  API-триггерный `Sync()` (как в Go's `handlers.go`) не имеет C-аналога
  вызова, только стартовый вызов из `main.c`.
- Подписки (`subscriptionRuleSets` в Go) не участвуют — Phase 7, как и
  было заложено в Phase 4.
- Аллокация локальных адресов сконфигурированных интерфейсов для
  port-remap собрана через POSIX `getifaddrs()`/`if_nametoindex()`,
  а не через отдельный rtnetlink dump — функционально эквивалентно
  списку адресов, который Go получает через
  `netlink.AddrList(link, nl.FAMILY_ALL)`, но не переиспользует `rtnl.h`;
  сочтено оправданным, так как `rtnl.h` не предоставлял (и не был обязан
  предоставлять) функцию листинга адресов интерфейса, а вводить её ради
  одного стартового вызова не оправдано.

## Вход Phase 6

HTTP/1.1 сервер (свой, ограниченный) + unix-сокет, таблица маршрутов,
JSON DTO-паритет, auth (crypt+JWT побайтово совместимые), раздача статики
скина по контракту §2–3; здесь же появится программный путь к
`mt_ruleset_t`'s `Sync()`/`Enable()`/`Disable()` при мутациях
групп/правил через API, аналогичный Go's `handlers.go`.
