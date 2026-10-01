# Phase 0 — отчёт (аудит и baseline)

Дата: 2026-07-22. Ветка: `claude/magitrickle-c-rewrite-phase-0-t7iwas`
(база: `develop` @ `8c8bd12`).

## Выполнено

- Полный аудит Go backend, сборки, упаковки, init-скриптов, CI и frontend
  toolchain; архитектура зафиксирована в `current-architecture.md`
  (с Mermaid-диаграммами: компоненты, DNS-запрос, reload, подписки,
  netfilter lifecycle).
- Compatibility contract (`compatibility-contract.md`): YAML, HTTP/Unix
  API, auth, DNS, кэш, правила, netfilter, подписки, платформа, упаковка —
  с указанием источника, существующих тестов, пробелов покрытия и способа
  проверки C-версии.
- Инвентаризация компонентов (`component-inventory.md`) с картой
  Go-пакет → C-модуль и оценкой рисков.
- Оценка зависимостей (`dependencies.md`) и toolchain-матрица
  (`toolchains.md`, 7 Entware + 33 OpenWrt targets из `config/`).
- Benchmark-инструментарий (`tools/bench/`: dnsstub, dnsload, genconfig,
  configbench, run_baseline.sh, run_tail.sh, summarize.py) и полный host
  baseline Go-версии (`performance-baseline.md` + raw в `baseline-raw/`).
- План миграции по фазам (`migration-plan.md`), журнал решений
  (`decisions.md`), `AGENTS.md`.
- Ключевые спорные поведения проверены эмпирически (yaml `enable`,
  duration-парсинг, ipset в контейнере, версия в configVersion).

## Проверенная архитектура

- Ядро: один процесс, goroutine-на-запрос DNS c глобальным семафором
  (default 100), RWMutex-состояние, iptables через save/restore batch,
  ipset через netlink, ip rule/route с автоаллокацией mark/table от
  0x4D616769, netlink watcher link/addr.
- DNS: UDP (pktinfo-ответы с правильного адреса) + TCP (ровно один запрос
  на соединение); сырые байты форвардятся upstream; hooks: fake PTR
  (NXDOMAIN), выпиливание AAAA из ответа клиенту (в ipset AAAA всё равно
  попадает), кэш A/AAAA/CNAME c reverse-alias BFS.
- `subnet`/`subnet6` не участвуют в DNS-матчинге — материализуются в ipset
  при Sync (подтверждено кодом; `IsMatch` → false).
- HTTP `/api/v1` + статика скинов; Unix socket — тот же роутер **без
  auth**; auth = shadow/passwd crypt (MD5/SHA256/SHA512) + самодельный
  JWT HS256 с ключом от (secret, passwordHash), срок 20 лет.
- Подписки: fetch (http/https, ≤5 редиректов 301/302), парсер с
  автоопределением типа, refresh с сохранением ID/enable, тикер 1 мин,
  rebuild всех subscription rule sets с rollback.
- Полный это описание — в `current-architecture.md`.

## Обнаруженные расхождения со спецификацией

1. **CLAUDE.md ошибался**: у моделей нет кастомного `UnmarshalYAML`;
   отсутствующее `enable` в YAML даёт **false**, а не true (проверено
   эмпирически; true подставляют только API-конвертеры при создании).
   CLAUDE.md поправлен, контракт зафиксирован по факту кода.
2. Спецификация (§6) верна в остальном; отдельные уточнения: redirect'ы
   подписок — только 301/302; `_kn` отличается только софтом (RCI,
   ignored-интерфейсы, ndm-hook, socat), ABI тот же.
3. Найденные особенности (не менять молча, зафиксированы):
   - `App.config` перезаписывается по SIGHUP без блокировки (data race в
     Go-версии) — C-версия сделает snapshot-swap (decisions D-10);
   - dev-сборка без версии пишет `configVersion: unattached`, который сама
     же отказывается загружать (`config unsupported version`) — проявилось
     в бенчмарке configbench; в релизных сборках не воспроизводится;
   - выход с кодом 0 даже при фатальной ошибке старта (decisions D-11);
   - SIGHUP-reload при отсутствующем ключе `subscriptions` очищает
     подписки, а отсутствующий ключ `groups` группы сохраняет —
     асимметрия зафиксирована в контракте;
   - regexp2 работает без таймаута → потенциальный ReDoS (C: лимиты
     PCRE2, задокументированное hardening-отличие).

## Созданные файлы

- `docs/c-rewrite/`: current-architecture.md, compatibility-contract.md,
  component-inventory.md, dependencies.md, toolchains.md,
  performance-baseline.md, benchmark-methodology.md, migration-plan.md,
  decisions.md, phase-0-report.md; `baseline-raw/` (environment.txt,
  results.jsonl, soak_rss.csv, soak_load.json, daemon.log).
- `tools/bench/`: go.mod/go.sum, dnsstub/, dnsload/, genconfig/,
  configbench/, run_baseline.sh, run_tail.sh, summarize.py, README.md.
- `AGENTS.md` (долговременные инструкции).
- Правка `CLAUDE.md` (одна строка — исправление неверного факта).

## Compatibility risks

- **regexp2 → PCRE2**: известный корпус совместим, но пользовательские
  .NET-конструкции (balancing groups, `\G`, conditionals) могут не
  скомпилироваться; политика: явная ошибка, без автоперезаписи.
- **YAML-байтовая форма**: yaml v2 (duration-строки, int=наносекунды,
  порядок ключей, `[]`) — гоняется differential-тестами; главный риск
  Phase 2.
- **JWT/crypt байтовая совместимость** (токены должны переживать апгрейд
  Go→C при том же secret).
- **ipset по netlink на старых ядрах 3.2/3.4** (протокольные ревизии) —
  нужен spike на реальном Keenetic.
- **Одна очередь на запрос/семафор**: поведение backpressure (клиент не
  получает ответа при недоступном upstream) должно сохраниться.
- **Асимметрия SIGHUP-reload** (groups vs subscriptions) — легко «починить
  случайно».
- **Список интерфейсов** (фильтр point-to-point, Keenetic-алиасы) —
  проверяемо только на устройстве.

## Dependency candidates

Итог (детали и альтернативы — `dependencies.md`): libyaml, cJSON, PCRE2,
libcurl (+TLS из фида), libmnl; vendored: uthash, тестовый фреймворк
(greatest.h/Unity), маленькие SHA-2/HMAC/crypt-реализации; DNS-парсер и
HTTP-сервер — собственные минимальные (HTTP — pending spike против
libmicrohttpd). Отказ: mongoose (GPLv2-only), libipset (GPLv2-only),
RE2/POSIX regex (семантика), libnl-3 (избыточен).

## Toolchain matrix

40 targets автоматически из `config/`: 7 Entware (glibc, ядра 3.2–3.10,
mips BE/LE softfloat, armv7 sf, aarch64; `_kn` = те же ABI + софт-отличия)
и 33 OpenWrt (musl, от armv4 `arm_fa526` до aarch64/riscv64/loongarch64,
mips64 n64). Требования: Entware glibc toolchain/sysroot, OpenWrt SDK
(TARGET_CC/CFLAGS/LDFLAGS), явный byte-order доступ (BE mips), запрет
`-march=native`/static glibc, фолбэк для `getrandom` на ядрах <3.17.
Детали и риски — `toolchains.md`.

## Go baseline

- окружение: контейнер 4 vCPU Intel Xeon 2.80 GHz, Linux 6.18.5,
  glibc 2.39, Go 1.24.7; upstream = локальный dnsstub; группа
  `enable:false` (ipset в контейнере недоступен — модуль ядра отсутствует);
  IPv6 нет → listen 0.0.0.0.
- команды: `sh tools/bench/run_baseline.sh` (LOAD_SEC=4 REPEATS=3
  SOAK_SEC=60), затем `sh tools/bench/run_tail.sh`; сводка —
  `python3 tools/bench/summarize.py docs/c-rewrite/baseline-raw/results.jsonl`.
- результаты (медианы): старт до первого ответа 51.6 мс; idle RSS 15.3 МБ /
  PSS 13.9 МБ, 10 потоков, idle CPU 0.1%; UDP 1k namespace-правил:
  ~33–35 krps @conc100 (p50 2.7 мс); 10k namespace: ~16 krps; 100k
  namespace: ~3.3 krps; 10k mixed (25% regex): **~1 krps при ~3.6 ядрах** —
  главный bottleneck; TCP ~0.5–0.7× UDP; CNAME-цепочка 10 → ~3 krps;
  upstream down → клиенту не отвечаем (все timeouts), CPU ~0; config
  load/save 10k правил: 75–178 / 119–351 мс; soak 60 с @18.5 krps:
  1.11 млн запросов, 0 ошибок, RSS стабилен 30→31.4 МБ.
- разброс: большинство ячеек ±5–8% (min–max по 3 повторам приведены в
  таблице `performance-baseline.md`); хост шумный (shared vCPU) — абсолюты
  индикативны, сравнение Go↔C будет back-to-back на одном хосте.
- device-замеры (реальный ipset/iptables, mips/arm): **pending** — скрипты
  параметризованы (`GROUP_ENABLE=1`), железа в этой среде нет; числа не
  выдумывались.

## Изменения production-кода

- отсутствуют (Go backend не тронут; правка CLAUDE.md — документация;
  bench-инструменты — отдельный модуль `tools/bench`).

## Проверки

- команда: `cd src/backend && go vet ./...`
  результат: ок (без замечаний).
- команда: `cd src/backend && go test ./...`
  результат: ок (groups, models, subscriptions, tests, recordsCache,
  magitrickle — все `ok`).
- команда: `go test -tags testing ./utils/iptables/`
  результат: ок.
- команда: `sh tools/bench/run_baseline.sh` + `run_tail.sh`
  результат: полный прогон, 183 записи в results.jsonl, soak без роста
  памяти.
- frontend-тесты не запускались (не требовались для Phase 0; frontend не
  менялся).

## Нерешённые вопросы, блокирующие совместимость

1. Совместимость PCRE2 с реальными пользовательскими regex (нужен корпус из
   боевых конфигов/подписок) — Phase 2 gate.
2. ipset-netlink на ядрах 3.4 Keenetic (`_kn`) — нужен device spike до
   выбора libmnl-пути как окончательного.
3. Политика exit-кода при фатальном старте (D-11) и асимметрия
   SIGHUP-reload — сохранить как есть или объявить осознанное отличие
   (решить в Phase 1, по умолчанию — сохранить как есть).
4. Байтовая форма YAML при сохранении: зафиксировать канонический fixture
   и подтвердить, что WebUI/пользовательские скрипты не зависят от
   отличий кавычек/отступов yaml v2 (Phase 2).
5. Точная семантика имён с не-ASCII/escape-последовательностями в miekg
   (представление имён для матчинга) — снять корпусом в Phase 3.

## Предлагаемые acceptance thresholds

(выведены из разброса baseline; полный список — в
`performance-baseline.md`): C ≥ 1.0× Go по rps на domain/namespace-ячейках
и ≥ 3× на mixed/regex @10k+; p50 ≤ Go, p99 ≤ 1.25× Go; idle RSS ≤ 7.5 МБ
(≤0.5×), under-load RSS ≤ 0.5×, дрейф RSS в soak ≤ +2%; CPU ≤ 0.7× Go при
равной нагрузке; старт ≤ 52 мс; config load/save ≤ 1.0× Go; ноль
ошибок/таймаутов там, где их ноль у Go. Пересчитать на тихом хосте перед
Phase 9.

## План Phase 1

1. Каркас `src/backend-c/` (структура из migration-plan/D-01), сборка C11
   с полным набором warning-флагов, `make test|sanitize|static_analysis`,
   кросс-сборочный скелет для 2 представительных targets (mipsel-3.4_kn
   glibc, mipsel_24kc musl) + CI-джоб.
2. Базовые модули: логгер, модель ошибок, lifecycle (init/destroy,
   partial-init unwind), event loop (epoll+timerfd+signalfd), bounded
   queue с метриками переполнения.
3. Vendored тест-фреймворк; каркас differential-раннера (Go и C бинарники
   на одинаковом входе, канонизированное сравнение).
4. Spikes из dependencies.md: PCRE2-корпус, libmnl ipset на старом ядре,
   epoll DNS echo на mipsel (qemu), libyaml emit-форма.
5. Решения D-02/D-06/D-11 подтвердить измерениями spike'ов; обновить
   decisions.md и migration-plan.md.
