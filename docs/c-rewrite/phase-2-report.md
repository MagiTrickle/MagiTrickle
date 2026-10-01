# Phase 2 — отчёт (модели, YAML, правила, подписки)

Дата: 2026-07-22. Ветка: `claude/magitrickle-c-rewrite-phase-0-t7iwas`.
Go backend не изменён (оракул использует его как есть).

## Выполнено

- **Модели** (`src/config/models.c`, `include/magitrickle/models.h`):
  rule/group/subscription/sub_rule/appconfig с явным ownership (все строки
  strdup/free, массивы владеют элементами), дефолты 1:1 с
  `constant.DefaultAppConfig`, нормализация цвета группы как в Go.
- **Duration** (`src/config/duration.c`): дословные порты
  `time.ParseDuration` и `Duration.String()` (включая µs/μs-юниты, дроби,
  переполнения, отрицательные значения). Раундтрип-векторы сверены с Go.
- **ID** (`src/config/id.c`): 4-байтовые ID, hex-парсер/форматер,
  `/dev/urandom` (без getrandom — ядра 3.2/3.4).
- **YAML-движок**:
  - `yaml_scalar.c` — порт резолвера yaml.v2 (`resolve.go`): bool-слова
    (`yes/On/…`), null-формы, int base-0 c `_`, yaml-флоты, base-60,
    timestamps, `isBase60Float` — общий для типизации load и квотирования
    save;
  - `yaml_load.c` — libyaml document API + оверлей-семантика Go
    `LoadConfig`: присутствующие поля поверх дефолтов, `configVersion`
    prefix "0.", legacy-нормализация (`timeout`<1ms⇒ms,
    `additionalTTL`<1s⇒s), bare int = наносекунды, quoted "8080" в порт =
    ошибка (как yaml.v2), integral float в uint = ок, absent `enable` =
    false, цвет нормализуется, дубликаты group/rule ID = ошибка,
    неизвестные ключи игнорируются, асимметрия groups/subscriptions
    сохранена;
  - `yaml_save.c` — эмиттер с порядком ключей и стилями yaml.v2
    (plain / double-quote для «нестроковых» строк типа `"12345678"`,
    `"666e0000"`, `"12:30:45"` / literal для `\n`; flow `[]` для пустых
    списков; single-quote отдаёт libyaml — тот же алгоритм, что в yaml.v2);
    атомарная запись: tmp+fsync+rename+fsync каталога, 0600.
- **Матчинг** (`src/rules/`): domain (exact), namespace (порт с
  dot-boundary и `.example.com`), wildcard — **дословный порт
  go-wildcard v2** (включая причуду: `.` в шаблоне матчит любой байт;
  зафиксировано в тестах), regex — PCRE2 CASELESS|UTF|UCP c
  match/depth-лимитами, invalid regex не матчит ничего; subnet/subnet6
  никогда не матчат. Групповой индекс `mt_matcher`: hash-set точных
  доменов + reversed-label trie для namespace + скан
  предкомпилированных wildcard/regex (OR-семантика группы).
- **Подписки** (`src/subscriptions/subparse.c`): токенизация
  `\n`/`\r`/`,`, комментарии, дедуп по (тип|текст), автоопределение типа
  (subnet6→subnet→namespace→domain→regex→wildcard; квирк Go сохранён:
  plain-домены всегда «namespace»), RefreshRules с сохранением
  ID/Enable/Type, sameRules (мультимножество), IsDue.
- **mt-configtool** — CLI-драйвер differential-тестов
  (resave/match/subparse/duration).
- **Differential-suites** (`tests/differential/run_diff.sh`), Go-оракул
  использует **реальные** production-пути (`App.LoadConfig`+`SaveConfig`,
  `models.Rule.IsMatch`, `subscriptions.ParseRules`):
  - config: 14 фикстур (полный конфиг, минимальный, пустые коллекции,
    отсутствующие enable, legacy-durations, неизвестные поля,
    bool-варианты, битый цвет+дубликат rule ID, неподдерживаемая версия,
    битый YAML, null-поля, неверный тип порта, дубликат group ID,
    «странные» строки: `"123"`, `'has: colon'`, `"12:30:45"`,
    многострочные) — **все байт-в-байт**; плюс defaults-паритет
    (отсутствующий файл) и cross-reload (C-вывод → Go → байт-в-байт);
  - match: 67 кейсов — идентично;
  - subparse: 15 правил — идентично;
  - regex/yaml spike-suites — без изменений, зелёные.

## Проверки

- `make CFLAGS_EXTRA=-Werror` (host) — 0 warnings.
- `make test` — 9 бинарников (44 теста), все Pass.
- `make sanitize` — ASan+UBSan, все Pass.
- `make static_analysis` — clang-tidy + cppcheck 2.13, 0 findings
  (в .clang-tidy дополнительно отключены шумовые
  multi-level-pointer/sizeof-expression проверки — идиоматичные
  malloc-паттерны).
- `sh tests/differential/run_diff.sh` — все suites зелёные (rc=0).
- Go-тесты backend не тронуты.

## Отличия от Go, зафиксированные в этой фазе

- `mt_rule_matcher` компилирует regex сразу (Go — лениво через
  `sync.Once`); наблюдаемое поведение идентично.
- Ошибки загрузки нормализуются в статус (оракул и C печатают "ERROR");
  тексты ошибок yaml.v2 не воспроизводятся (не контракт).
- Атомарная запись файла — улучшение, разрешённое спецификацией §17
  (байтовое содержимое идентично).
- Regex-валидация подписок использует PCRE2 (D-07): списки с паттернами
  из известных классов расхождений могут классифицироваться иначе, чем в
  Go; корпус subparse это мониторит.

## Нерешённое / перенесённое

- Расширение regex-корпуса реальными пользовательскими списками — по мере
  доступа к боевым конфигам (заявка в phase-0-report остаётся).
- `strings.TrimSpace` юникод-пробелы в парсере подписок (C — ASCII);
  расхождение возможно только для экзотических списков, ловится корпусом.
- Производительность матчинга (индексы vs линейный скан Go) — замеры в
  Phase 4, когда появится DNS-путь.

## Вход Phase 3

DNS transport (epoll UDP c pktinfo, TCP one-query-per-conn), upstream
pools, correlation+timeouts, wire-парсер с fuzz-таргетами, differential
DNS-корпус (семантическое сравнение), первые замеры против Go baseline.
