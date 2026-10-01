# Phase 1 — отчёт (фундамент C backend)

Дата: 2026-07-22. Ветка: `claude/magitrickle-c-rewrite-phase-0-t7iwas`.
Go backend не тронут.

## Выполнено

- Каркас `src/backend-c/` (структура из D-01): публичные заголовки в
  `include/magitrickle/`, модули `main/ core/ logging/ platform/ util/`,
  тесты `tests/{unit,vendor,differential}`, spikes.
- Сборочная система: C11, полный warning-набор из спецификации §20,
  `make / test / sanitize / static_analysis / clean`, `CFLAGS_EXTRA=-Werror`
  в CI, кросс-сборка через `CROSS_COMPILE`/`SYSROOT`
  (проверено: mipsel, ELF 32-bit MIPS, min kernel 3.2).
- Модули ядра:
  - `err` — единая модель ошибок (`mt_err_t`) + маппинг errno;
  - `logging` — потокобезопасный консольный логгер, уровни и fallback
    идентичны Go `setupLogging` (включая unknown→info);
  - `util/queue` — bounded MPMC-очередь: политики REJECT/DROP_OLDEST,
    счётчик dropped, timeout-pop, close-семантика (spec §12);
  - `core/lifecycle` — симметричные init/destroy c размоткой частичной
    инициализации в обратном порядке;
  - `platform/loop` — event loop: epoll (level-triggered) + timerfd
    (one-shot/периодические) + signalfd + eventfd-post из других потоков
    (bounded, MT_ERR_LIMIT при переполнении). GNU/Linux API только в
    platform-слое (D-14).
  - `main` — скелет демона: логгер, loop, TERM/INT → graceful stop,
    HUP-заглушка (reload в Phase 2), `--version`.
- Тесты: 19 unit-кейсов (greatest.h v1.5.0, ISC, завендорен) — очередь
  (вкл. конкурентный producer/consumer), lifecycle, логгер (формат,
  фильтрация, парсинг уровней), loop (таймеры, fd-события, cross-thread
  post, отмена таймера). Все зелёные, включая ASan+UBSan прогон.
- Статический анализ: clang-tidy (конфиг `.clang-tidy`) + cppcheck —
  0 замечаний.
- Spike PCRE2 vs regexp2: **44/47 идентично**, 3 задокументированных
  расхождения (D-07 принят). Corpus + known_divergences зафиксированы и
  входят в differential-раннер.
- Spike libyaml vs yaml.v2: **байт-в-байт идентичный** вывод конфига
  (D-04 принят).
- Differential-каркас `tests/differential/run_diff.sh`: сейчас гоняет обе
  spike-пары (regex-расхождения сверяются с замороженным известным
  набором), расширяется по фазам.
- CI: `.github/workflows/check-c.yml` — build(-Werror), test, sanitize,
  static_analysis, differential, mipsel cross-build; триггер только по
  путям `src/backend-c/**`.
- Корневой Makefile: добавлена цель `build_backend_c` (существующие цели
  не тронуты).

## Решения, подтверждённые измерениями

- D-04 (libyaml) — accepted: байтовая совместимость достижима.
- D-07 (PCRE2) — accepted: расхождения только POSIX-классы/possessive
  (в пользу C) и .NET balancing groups (compile error, политика — явный
  отказ).
- D-02 (epoll loop) — реализован каркас; финальное подтверждение
  производительности отложено до Phase 3 (DNS transport) по плану.

## Не сделано (осознанно, вне рамок Phase 1)

- Spike libmnl/ipset на старом ядре — требует устройства (в контейнере
  нет модуля ip_set; отмечено в phase-0-report как блокер Phase 5).
- Spike epoll DNS echo на mipsel qemu — нет qemu в среде; кросс-сборка
  скелета выполнена, echo-замер перенесён в Phase 3.
- Настоящие Entware/OpenWrt toolchain-дескрипторы (Phase 1 использовал
  Debian mipsel-gcc как проверку кросс-механики Makefile; пиновка
  реальных toolchain'ов — вход Phase 8, зафиксировано в toolchains.md).

## Проверки

- `make CFLAGS_EXTRA=-Werror` — ок (0 warnings).
- `make test` — 5 бинарников, 19 тестов, все Pass.
- `make sanitize` — ASan+UBSan, все Pass.
- `make static_analysis` — clang-tidy + cppcheck, 0 findings.
- `make CROSS_COMPILE=mipsel-linux-gnu- CFLAGS_EXTRA=-Werror` — ок.
- `sh tests/differential/run_diff.sh` — обе suites зелёные.
- `cd src/backend && go test ./...` — Go-тесты не задеты (пройдены в
  Phase 0; код Go не менялся).

## Вход Phase 2

Модели (group/rule/subscription/appconfig), YAML load/save на libyaml с
подтверждённой байтовой формой, rule matching (hash + reversed-trie +
скомпилированный PCRE2), парсер/refresh подписок, differential-тесты
load/save Go↔C. Расширить regex-корпус реальными списками пользователей.
