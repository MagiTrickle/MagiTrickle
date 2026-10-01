# Phase 6 — отчёт (API & WebUI)

Дата: 2026-07-23. Ветка: `claude/magitrickle-c-rewrite-phase-0-t7iwas`.
Go backend не изменён.

## Выполнено

- **cJSON + JSON helpers** (`src/api/json.c`, `include/magitrickle/json.h`)
  — тонкая обёртка над cJSON (динамическая линковка, D-05 revisited):
  `mt_json_error`/`mt_json_dump`, зафиксировано как **не цель**
  побайтовое совпадение с `encoding/json` (HTML-экранирование и т.п.) —
  контракт проверяется структурным сравнением.
- **Криптопримитивы** (`src/crypto/{md5,sha256,sha512,hmac,base64,
  crypt,jwt}.c`) — собственные MD5 (RFC 1321), SHA-256/512 (FIPS 180-4),
  HMAC-SHA256 (RFC 2104), base64 (RFC 4648 + URL-safe), crypt(3)
  `$1$`/`$5$`/`$6$` (побайтовый порт `api/auth/crypt.go` — единственный
  случай в проекте, где близкий построчный порт оправдан, см. D-23) и
  HS256 JWT (порт `api/auth/jwt.go`). Референсные векторы захвачены из
  реального Go через временные `TestGenVectors`/`TestGenJWTVectors`
  (созданы, прогнаны, удалены — никогда не коммитились).
- **HTTP/1.1 сервер** (`src/api/httpd.c`, `include/magitrickle/httpd.h`)
  — собственный, ограниченный (D-06 revisited): keep-alive, `{name}`-роутер,
  общее ядро для TCP и Unix-сокета, `mt_httpd_set_middleware`/
  `set_not_found`. Нормализация `.`/`.."`-сегментов пути
  (`path_clean()`, добавлено в этой фазе — см. ниже, security-фикс)
  теперь соответствует изначально заявленному в хедере контракту
  "matches Go's path.Clean".
- **Auth** (`src/api/auth.c`, `include/magitrickle/auth.h`) — crypt(3)
  проверка пароля из `/etc/shadow` (fallback `/etc/passwd`), JWT
  токен, ключ подписи = hex(HMAC-SHA256(app_secret, passwordHash))
  (смена пароля инвалидирует все токены), app_secret на диске
  (`<state_dir>/auth_secret`, 0600/0700), 20-летний срок жизни токена
  через гражданский календарь Хауарда Хиннанта (без `timegm(3)`, см.
  D-24). Middleware гейтится точно как в Go: `/api/` + auth enabled +
  не `/api/v1/auth`.
- **App layer** (`src/api/app.c`, `include/magitrickle/app.h`) — группы
  (`mt_app_add_group`/`clear_groups`/`remove_group_by_index`/`by_id`,
  с той же асимметрией `ClearGroups`-disables-vs-`RemoveGroupBy*`-doesn't,
  что и в Go), интерфейсы (`getifaddrs`, дедуп, `IFF_POINTOPOINT`),
  сохранение конфига, `ForceCommitIPTables`. `cfg->groups` — единый живой
  реестр групп (не Go-стиль реконструкции при сохранении, см. D-25).
  Позже дополнен CRUD подписок (`mt_app_add_subscription`/
  `replace_subscriptions`/`remove_subscription_by_id`) — без runtime
  netfilter-состояния (см. D-28).
- **Groups/Rules HTTP handlers** (`src/api/groups.c`) — полный
  GET/PUT/POST/DELETE для `/groups`, `/groups/{id}`,
  `/groups/{id}/rules[...]`: нормализация цвета, две разные семантики
  переиспользования ID правил (мягкая — `RuleFromReq`-эквивалент для
  create/nested; строгая — только для bulk `PutRules`, 404 на
  непойманный id), enable/disable/sync-хореография вокруг PUT/DELETE
  (см. D-26). Добавлен `mt_ruleset_group_mut()` (ruleset.h/.c) для
  редактирования группы на месте с сохранением identity внутри
  `cfg->groups`.
- **System + static serving** (`src/api/system.c`, `src/api/
  staticfiles.c`) — `ListInterfaces`/`SaveConfig`/`NetfilterDHook`;
  раздача скина точно по алгоритму `http.go` (до 2 попыток stat с
  `index.html`-фоллбэком, content-type по расширению, HTML-плейсхолдер
  на `/`). Найден и исправлен реальный security-гэп: `mt_http_req_path()`
  заявлял в хедере "path-cleaned", но никогда не схлопывал `.`/`..` —
  добавлен `path_clean()` в `httpd.c`, что делает раздачу статики
  безопасной от выхода за пределы `skins_dir` (см. D-27).
- **Subscriptions CRUD (non-fetch)** (`src/api/subscriptions_api.c`) —
  `GET`/`PUT`/`POST /subscriptions`, `DELETE /subscriptions/{id}`.
  Сознательно НЕ реализованы `POST .../sync` и
  `GET /subscriptions/rules?url=` (нужен libcurl — Phase 7); ни один
  роут не зарегистрирован, а не заглушен фейковым успехом. Сохранены
  две реальные особенности Go (не "исправлены"): противоположный
  дефолт `?save=` (opt-out, а не opt-in как у групп) и то, что
  `ensureUniqueSubscriptionIDs`/`...RuleIDs` — тихий фиксап, а не
  валидация (см. D-28).
- **`main.c`** — переписан на `mt_app_t` вместо инлайновой обвязки
  Phase 5 над сырым массивом `mt_ruleset_t**`; поднимает Unix-сокет
  (всегда, полный v1-роутер) и TCP WebUI (по конфигу, + auth middleware
  + static fallback), в точности как `http.go`/`unixsocket.go`.
- **Дифференциальные наборы**:
  - `tests/differential/run_http_diff.sh` + `http_contract/contract.py`
    — реальный демон Go vs реальный `magitrickled-c`, 37 шагов (auth,
    полный CRUD групп/правил включая bulk PUT, system-эндпоинты,
    CRUD подписок, Unix-сокет), побайтовое сравнение с "learn-then-redact"
    для случайных ID (см. D-29). Подключён как финальный шаг
    `run_diff.sh`.
  - `tests/differential/run_e2e_diff.sh` — реальный Playwright-набор
    `tests/e2e/*.spec.ts` (45 тестов, немодифицированные) против
    `magitrickled-c`, раздающего собранный production-фронтенд как
    скин `default` (см. D-30).

## Решения (decisions.md)

- **D-05 (revisited)** — cJSON, побайтовая идентичность сериализации не
  цель.
- **D-23** — собственные крипто-примитивы вместо OpenSSL/mbedtls;
  близкий порт crypt(3)/JWT оправдан именно для побайтовых алгоритмов.
- **D-06 (revisited)** — собственный ограниченный HTTP/1.1 сервер.
- **D-24** — модуль путей платформы; календарная арифметика JWT без
  `timegm(3)`.
- **D-25** — `cfg->groups` как единый живой реестр (не Go-реконструкция).
- **D-26** — дизайн groups.c/HTTP-хендлеров, `mt_ruleset_group_mut`,
  мягкое/строгое переиспользование ID правил, недостающая пока
  синхронизация подписок при bulk PUT групп (задокументированный гэп).
- **D-27** — system-эндпоинты, раздача статики, найденный и исправленный
  `path_clean()`-гэп (path traversal), рефакторинг `main.c` на `mt_app_t`.
- **D-28** — CRUD подписок без runtime netfilter-состояния (реальный,
  весомый гэп, а не паритет с группами); две сохранённые Go-особенности.
- **D-29** — HTTP-контрактный дифф-набор Go-демон vs C-демон; "learn,
  then redact" для случайных ID; нормализация текста ошибок (не цель
  побайтового совпадения).
- **D-30** — Playwright e2e против `magitrickled-c`; два environment-специфичных
  фикса (Chromium executablePath, origin для clipboard-permission),
  ни один не тронул сами спеки.

## Проверки

- `make test` — **25 бинарников юнит-тестов** (было 12 после Phase 5;
  новые в этой фазе: test_json, test_hash, test_crypt, test_jwt,
  test_httpd, test_auth, test_app, test_groups, test_system,
  test_staticfiles, test_subscriptions_api) — все Pass.
- `make static_analysis` (clang-tidy + cppcheck по всему `LIB_SRCS`+
  `MAIN_SRC`+тулы+test-support) — **0 warnings**.
- `make sanitize` (ASan+UBSan) — 0 находок по всем 25 бинарникам.
- `tests/differential/run_diff.sh` (полный прогон, все фазы) — все
  дифф-наборы зелёные, включая новый `run_http_diff.sh` (37/37 шагов
  идентичны между реальным Go-демоном и `magitrickled-c`), прогнан
  дважды подряд без остаточного состояния (`iptables -L`/`-t nat -L`
  пусто, нет висячих сокетов/PID-файлов).
- `run_e2e_diff.sh` (Playwright `tests/e2e/*.spec.ts`, 45 тестов,
  немодифицированные) против `magitrickled-c`, раздающего реальный
  production-фронтенд — **45/45 passed**, прогнан дважды подряд с
  чистого состояния.
- **Сквозной smoke живого демона** (вне автоматических наборов, в ходе
  разработки задач #34/#35/#36): реальный `curl` по TCP и Unix-сокету
  против групп/правил/system/subscriptions-эндпоинтов, включая
  наблюдение реального 500 (`ip_set` недоступен в песочнице, тот же
  Phase-0-факт) при попытке включить группу/подписку с `enable:true`,
  и подтверждение, что неудачная попытка не оставляет частично
  добавленную сущность — то же поведение, что уже проверено юнит-тестом
  `test_app.c`'s `add_group_while_running_rolls_back_on_failure`.

## Нерешённое / перенесённое

- Синхронизация subscription rule sets при bulk `PUT /groups`
  (Go's `SyncSubscriptionRuleSets`) — no-op в C-порте, так как
  подписки ещё не имеют runtime netfilter-состояния (см. D-28); будет
  закрыто вместе с Phase 7's subscription fetch/sync.
- `POST /subscriptions/{id}/sync` и `GET /subscriptions/rules?url=` —
  не реализованы (нужен libcurl), роуты не зарегистрированы — Phase 7.
- Keenetic RCI friendly-name lookup для интерфейсов (`entware_kn`) —
  каждый интерфейс возвращает пустое имя (совпадает с Go's
  `DummyRouterSpecificAPI`, единственным фоллбэком, который использует
  любая не-`entware_kn` сборка) — отложено на Phase 8 packaging.
- Реальный per-platform `MT_APP_SHARE_DIR`/`MT_APP_STATE_DIR`/
  `MT_SOCK_PATH` (сейчас compile-time дефолты из `paths.h`, совпадающие
  с `path_default.go`) — реальная per-platform подстановка через build
  flags — Phase 8 packaging, как и было заложено в исходном плане этой
  фазы. **(Done — см. D-49: `PLATFORM=entware|openwrt` → `-DMT_PLATFORM_*`
  выбирает набор путей в `paths.h`; без этого Entware-пакеты собирались с
  `/var/...` вместо `/opt/...`.)**
- Cross-compilation под реальные Entware/OpenWrt тулчейны не
  выполнялась в этой песочнице (тулчейны недоступны) — ровно та же
  ситуация, что и в Phase 4/5 (тулчейны/CI-матрица — предмет Phase 8);
  весь host-side риск (типы, warnings, sanitizers) покрыт максимально
  строгими флагами (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow
  ...`) на протяжении всей фазы.

## Вход Phase 7

Подписки: libcurl fetch с точной семантикой редиректов + ограничение
размера; планировщик на timerfd; rebuild/rollback flows; SIGHUP reload;
save-after-sync — на этой базе `mt_app_t`'s подписочный CRUD (уже
готовый config-mutation слой из этой фазы) получит реальный runtime
(`mt_ruleset_t`-эквивалент на подписку, синхронизация snapshot'а
DNS-пайплайна), а два отложенных HTTP-эндпоинта
(`POST .../sync`, `GET .../rules?url=`) наконец получат реализацию.
