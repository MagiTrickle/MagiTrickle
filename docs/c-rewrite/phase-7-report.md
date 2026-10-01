# Phase 7 — отчёт (subscriptions & full integration)

Дата: 2026-07-23. Ветка: `claude/magitrickle-c-rewrite-phase-0-t7iwas`.
Go backend не изменён.

## Выполнено

- **libcurl fetch** (`src/subscriptions/fetch.c`, `include/magitrickle/
  sub_fetch.h`) — `mt_sub_fetch_list()`: точная семантика редиректов
  Go's `FetchList` (только 301/302 — "редирект, который стоит
  проследовать", любой другой 3xx — терминальная non-2xx ошибка, тот же
  Go-квирк, не "исправлен"), максимум 5 редиректов с тем же порядком
  проверок, что и Go's `for redirects := 0; ; redirects++`, обнаружение
  циклов через маленький фиксированный массив посещённых URL.
  `curl_url()`/`curl_url_set/get` вместо самодельного URL-парсера;
  `CURLINFO_REDIRECT_URL` уже резолвит относительный `Location` в
  абсолютный — как Go's `ResolveReference`. `MT_SUB_FETCH_MAX_BODY_BYTES`
  (8 МиБ) — новое C-side hardening, не порт поведения (у Go's
  `io.ReadAll` предела нет вовсе). `mt_sub_fetch_global_init`/`_cleanup`
  — ответственность вызывающего (main.c), как того требует контракт
  libcurl (см. D-31).
- **Subscription runtime rule sets** (`src/subscriptions/runtime.c`,
  `include/magitrickle/sub_runtime.h`) — `mt_sub_runtime_group()`
  синтезирует независимый `mt_group_t*` из `mt_subscription_t`
  (зеркалирует Go's `subscriptionAsRuntimeRuleSet`: `enable` = `sub.Enable
  && iface != ""`), скармливаемый уже существующему, немодифицированному
  `mt_ruleset_new()` — синтез вместо обобщения `ruleset.c` (см. D-32).
  `mt_app_t` получил отдельный массив `sub_rulesets`/`sub_synth_groups`
  (rebuild-wholesale + rollback-on-failure, зеркалирует
  `syncSubscriptionRuleSetsLocked`), плюс итерацию/поиск для
  netlink-вотчера и DNS match sink.
- **Найден и исправлен реальный Phase-6 регресс**: DNS-matching snapshot
  строился ровно один раз при старте демона и никогда не
  перестраивался — любая HTTP-мутация группы/подписки обновляла
  netfilter, но никогда DNS-сопоставление. Добавлен
  `mt_app_republish_dns_snapshot()`, вызываемый изнутри всех мутаторов
  `mt_app_t` и явно из groups.c-хендлеров, редактирующих группу на месте
  (см. D-32). Регрессионный тест в `test_groups.c` доказывает фикс через
  реальный HTTP-путь.
- **Sync flows** (`mt_app_sync_subscription_by_id`/
  `mt_app_sync_due_subscriptions`, `src/api/app.c`) — порты
  `SyncSubscriptionByID`/`SyncDueSubscriptions`: fetch → `RefreshRules`
  → rebuild-if-changed → rollback-on-failure, переиспользуя Phase 2's
  `mt_sub_refresh_rules`/`mt_sub_same_rules`/`mt_sub_is_due`. Новый код
  `MT_ERR_UPSTREAM`, чтобы отличать неудачу fetch (502) от неудачи
  rebuild (500) на уровне HTTP. Задокументированное, не тайное,
  ограничение: обе функции делают **блокирующий** сетевой fetch на
  вызывающем потоке — сегодня это всегда единственный event-loop поток
  (см. D-33).
- **HTTP-эндпоинты** — `POST /api/v1/subscriptions/{id}/sync` и
  `GET /api/v1/subscriptions/rules?url=`, два маршрута, сознательно
  оставленные незарегистрированными в Phase 6 (D-28). Маппинг ошибок
  зеркалирует Go's `switch { errors.Is(...) }` (404/400/502/500);
  save на sync-эндпоинте зависит от `changed`, в отличие от
  безусловного save остальных subscription-хендлеров (см. D-34).
  HTTP-контрактный дифф-набор расширен стаб-сервером списка подписок
  и новыми шагами; прогнан вживую в этой песочнице — 44/44 шага
  идентичны.
- **Auto-update scheduler + SIGHUP reload** (`main.c`) — таймер на
  `mt_loop_add_timer` (1 минута, `initial_ms=0` даёт немедленный первый
  тик — порт `StartSubscriptionAutoUpdate`), вызывающий
  `mt_app_sync_due_subscriptions` + save-on-change. SIGHUP теперь
  реально перечитывает YAML и применяет: (1) группы/подписки целиком
  (`groups_present`/`subscriptions_present`), (2) только те app-level
  настройки, которые Go сам перечитывает вживую на каждый DNS-запрос/
  запись (`DisableFakePTR`/`DisableDropAAAA`, `AdditionalTTL`) — новые
  сеттеры `mt_dnsproxy_set_disable_flags`/`mt_dns_pipeline_set_
  additional_ttl`. Остальные app-level поля — задокументированная
  граница объёма (см. D-35), не пропуск.
- **End-to-end fault injection + soak** (`tools/bench/
  run_c_subscription_fault_soak.sh` + `subscription_fault_stub.py`) —
  реальный `magitrickled-c` против стаб-DNS-апстрима и стаб-сервера
  списка подписок, целенаправленно бьющего во все режимы отказа
  (redirect loop, non-2xx, oversized body, connection refused) плюс
  одну постоянно меняющуюся подписку, всё на 4-секундном интервале,
  под нагрузкой DNS-трафика и периодическими SIGHUP. Ограниченная
  длительность (90 с) — замена плановому 24-часовому soak,
  задокументированная, а не тихая (см. D-36).
- **Найден и исправлен реальный memory leak**: прогон soak под
  ASan/UBSan показал `LeakSanitizer`-находку. Изолирован серией
  автономных репро до `mt_app_sync_due_subscriptions`'s success-пути в
  `app.c` — при реальном изменении правил подписки код сохранял старый
  указатель на правила в rollback-структуру (для отката), но на
  **успешном** пути освобождал только сам rollback-массив, никогда
  — сами старые (уже замещённые) правила. Каждый настоящий цикл
  "auto-update нашёл новый контент" бесконечно "терял" один массив
  правил. Сестринская функция (`mt_app_sync_subscription_by_id`, тот же
  Phase 7, задача #42) этот момент уже делала правильно — узкий,
  однофункциональный пропуск, не системный паттерн. Исправлено;
  добавлен регрессионный тест в `test_sub_sync.c` (ценность — только
  под `make sanitize`, функционально тест всегда проходил).

## Решения (decisions.md)

- **D-31** — libcurl fetch (`mt_sub_fetch_list`): точная семантика
  редиректов, `MT_SUB_FETCH_MAX_BODY_BYTES` как hardening,
  `curl_global_init`/`cleanup` — ответственность вызывающего.
- **D-32** — subscription runtime rule sets через синтез
  (`mt_sub_runtime_group`), не обобщение `ruleset.c`; параллельный
  владеющий массив `sub_synth_groups`; найденный и исправленный
  Phase-6 DNS-snapshot-регресс (относится и к группам, и к подпискам).
- **D-33** — `mt_app_sync_subscription_by_id`/
  `mt_app_sync_due_subscriptions`; отсутствие result-copy структуры в
  API (безопасно на однопоточной модели); новый `MT_ERR_UPSTREAM`;
  задокументированный компромисс блокирующего fetch на event-loop
  потоке (worker-thread редизайн — будущая работа, названная ещё в
  D-02/D-17).
- **D-34** — HTTP-эндпоинты sync/rules-preview; сохранён save,
  зависящий от `changed`; расширенный HTTP-контрактный дифф-набор;
  реальный self-deadlock, найденный при написании тестов (стаб-сервер
  и API-сервер на одном event-loop потоке).
- **D-35** — auto-update таймер (timerfd) + SIGHUP reload; явная
  граница объёма — какие app-level настройки реально "живые" в Go
  (и, соответственно, в C), а какие только обновляют неиспользуемую
  копию в памяти в обоих бэкендах.
- **D-36** — fault-injection + soak tooling; найденный и исправленный
  реальный memory leak (`mt_app_sync_due_subscriptions`'s success
  path); подтверждённый, сознательно не тронутый pre-existing Go баг
  (`configVersion` "unattached" не проходит собственную же проверку
  версии при перезагрузке — воспроизведён побайтово, не "исправлен");
  найденный и исправленный timing race в самом тестовом скрипте.

## Проверки

- `make test` — **28 бинарников юнит-тестов** (было 25 после Phase 6;
  новые: test_sub_fetch, test_sub_sync; расширены test_groups,
  test_rulesnap, test_subscriptions_api) — все Pass.
- `make static_analysis` (clang-tidy + cppcheck) — **0 warnings**,
  проверено многократно на протяжении фазы, финально — после
  memory-leak-фикса.
- `make sanitize` (ASan+UBSan) — 0 находок по всем 28 бинарникам,
  финально — после memory-leak-фикса.
- `tests/differential/run_http_diff.sh` (расширен стаб-сервером
  списка подписок и sync/rules-preview шагами) — **44/44 шага**
  идентичны между реальным Go-демоном и `magitrickled-c`, прогнан в
  этой песочнице (root + реальный iptables доступны).
- **Ручная сквозная проверка живого демона** (SIGHUP reload, вне
  автоматических наборов — для `main.c` нет отдельного unit-теста):
  группа добавлена в конфиг на диске, `SIGHUP`, `GET /api/v1/groups`
  немедленно показывает перезагруженную группу по HTTP; повторено с
  подпиской; чистый `SIGTERM`. Под sanitize-сборкой — 0 находок через
  несколько циклов reload.
- **End-to-end fault-injection soak** (`run_c_subscription_fault_soak.sh`,
  90 с, замена плановому 24 ч): демон жив всё время, несколько
  реальных SIGHUP reload под конкурентной DNS-нагрузкой и due-подписками,
  ~20-30k rps устойчивой DNS-нагрузки, чистый `SIGTERM`. Под
  ASan+UBSan (после фикса) — **0 находок** через 4 SIGHUP-reload и
  20+ реальных циклов sync подписок, включая повторные изменения
  контента — именно тот сценарий, что тёк до фикса.

## Нерешённое / перенесённое

- Worker-thread редизайн для блокирующего libcurl fetch (не блокировать
  event-loop поток на время сетевого запроса) — названо ещё в
  D-02/D-17, требует также рефакторинга `httpd.c` под отложенные
  HTTP-ответы; вынесено за рамки Phase 7 по объёму (см. D-33/D-34).
- Реальный 24-часовой host soak — заменён 90-секундным bounded-прогоном
  (см. D-36); при наличии внешнего хоста с этой возможностью стоит
  прогнать `run_c_subscription_fault_soak.sh` с `DURATION=86400`.
- Pre-existing Go баг (`configVersion` "unattached" не проходит
  собственную проверку версии при перезагрузке после auto-save) —
  подтверждён, сознательно не тронут ни в Go, ни в C (см. D-36);
  затрагивает оба бэкенда одинаково при отсутствии инъекции реальной
  версии на сборке (сегодня не делается ни в одном CI-файле этого
  репозитория).
- Cross-compilation под реальные Entware/OpenWrt тулчейны не
  выполнялась в этой песочнице (тулчейны недоступны) — та же ситуация,
  что и в Phase 4/5/6 (тулчейны/CI-матрица — предмет Phase 8).

## Вход Phase 8

Backend полностью переключаем на C для сборки пакетов: `make
build_backend` для всех 40 целей (матрица из `config/`), ipk/apk
упаковка, init-скрипты, `_kn`-файлы без изменений (Depends обновить:
добавить libyaml/cjson/pcre2/libcurl/libmnl), тесты
upgrade Go-пакет → C-пакет (конфиг сохраняется, сервис
перезапускается, netfilter-состояние чистится/перестраивается),
решение по UPX — по замерам. Вся Phase 7 функциональность (subscription
fetch/sync/auto-update/reload) уже прошла дифференциальную и
fault-injection проверку и готова войти в пакет без дополнительной
доработки.
