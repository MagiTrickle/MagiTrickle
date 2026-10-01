# Phase 3 — отчёт (DNS transport, wire-парсер, fuzz, differential)

Дата: 2026-07-22. Ветка: `claude/magitrickle-c-rewrite-phase-0-t7iwas`.
Go backend не изменён (miekg/dns используется как оракул).

## Выполнено

- **DNS wire-кодек** (`src/dns/wire.c`, `include/magitrickle/dnswire.h`):
  bounds-checked парсер с явным сетевым порядком байт (big-endian mips —
  first-class), защита от: выхода за буфер, integer overflow, compression
  loops (лимит прыжков + указатель обязан быть внутри сообщения),
  слишком длинных имён (≤255), некорректных длин label (≤63), лжи в
  section counts, некорректной TCP-длины. Никаких cast'ов wire-буфера в
  структуру. Каноническая (декомпрессированная) rdata для компрессируемых
  типов (CNAME/NS/SOA/MX/PTR/SRV…) — как `ToRFC3597` в miekg. Упаковщик
  без компрессии. Хелперы hot-path: детект single-PTR и синтез fake-PTR
  NXDOMAIN, оба на сырых байтах. Воспроизведено документированное
  послабление miekg: сообщение ровно из 12 байт парсится как header-only
  (пустые секции, счётчики игнорируются).
- **DNS differential** (`tests/differential`): Go-оракул на **miekg/dns**
  (та же библиотека, что в backend) даёт канонический, независимый от
  компрессии дамп; генератор корпуса на miekg (валидные + краевые +
  вручную собранные malformed). Три режима — `dump`, `stripaaaa`
  (модель response-hook: strip AAAA + repack + reparse), `ptrcheck` —
  **20 сообщений идентичны байт-в-байт** C и Go по всем трём режимам,
  включая A/AAAA/CNAME-цепочки, NXDOMAIN+SOA, MX+additional, EDNS OPT и
  malformed.
- **Fuzz** (`tests/fuzz`, libFuzzer): таргеты `fuzz_dns_parse`
  (parse+pack+strip+reparse+name-render) и `fuzz_fake_ptr`; seed-корпус
  из валидных сообщений; `make fuzz` c ASan+UBSan. Smoke — **300k
  прогонов/таргет без крашей, утечек, OOB и зависаний**. Добавлен в CI.
- **epoll DNS-прокси** (`src/dns/proxy.c`,
  `include/magitrickle/dnsproxy.h`): полностью event-driven на общем
  `mt_loop`, **без потока-на-запрос**. UDP — один неблокирующий сокет,
  ответ отправляется с исходным адресом назначения через
  `IP_PKTINFO`/`IPV6_PKTINFO` (как Go); пул подключённых upstream-сокетов.
  TCP — accept4, ровно **один запрос на соединение** (контракт), затем
  закрытие; неблокирующий connect к upstream (EINPROGRESS→EPOLLOUT).
  Backpressure: ограниченный бюджет in-flight (`max_concurrent`) — при
  переполнении UDP-датаграммы отбрасываются (клиент повторяет), TCP-accept
  тормозится; счётчик dropped. Per-request дедлайны на timerfd. Хуки как в
  `dns.go`: fake-PTR локальный ответ, strip-AAAA + repack, callback для
  будущего кэша/матчинга (Phase 4).
- **Демон** (`src/main/main.c`): грузит YAML-конфиг (defaults+overlay),
  ставит уровень логирования, поднимает прокси на loop, graceful shutdown
  по TERM/INT, HUP-заглушка. `--config <path>`.
- **Сборка**: wire+proxy — в dependency-free CORE (кросс-компилируются
  везде); полный демон линкует config-слой + libyaml/pcre2 (грузит
  конфиг); кросс-скелет собирает только CORE до Phase 8 sysroots.

## Проверки

- `make CFLAGS_EXTRA=-Werror` (host) — 0 warnings.
- `make test` — 10 бинарников (test_dnswire +7 кейсов), все Pass.
- `make sanitize` — ASan+UBSan, все Pass.
- `make static_analysis` — clang-tidy + cppcheck, 0 findings.
- `make fuzz FUZZ_RUNS=300000` — оба таргета чисто.
- `make CROSS_COMPILE=mipsel-linux-gnu- CFLAGS_EXTRA=-Werror` — ок.
- `sh tests/differential/run_diff.sh` — все suites зелёные, включая новые
  DNS-режимы.
- **ASan/UBSan-прогон демона под нагрузкой** (UDP+TCP+fake-PTR+
  upstream-down, затем SIGTERM): 0 ошибок, 0 утечек, чистое завершение —
  транспорт memory-safe под нагрузкой.
- Функциональный smoke (`tools/bench/run_c_smoke.sh`): UDP/TCP отвечают
  корректно, fake-PTR даёт `qr=1 ra=1 rcode=3 ancount=0` (точно как Go).

## Benchmark: C-прокси vs Go baseline

Одинаковый хост/dnsstub/dnsload/методология. Медианы по 3 повторам, 4 c.

**Важная оговорка (честное сравнение):** Phase 3 C-демон выполняет
transport + разбор DNS + хуки, но **ещё не делает rule matching / кэш /
ipset** (Phase 4/5). Go baseline запускался с отключённой группой (ipset
не пишется), но **на каждый ответ прогонял матчинг по всем правилам**.
Поэтому часть выигрыша C отражает пока не реализованную работу — это
сравнение транспортного слоя, не полного конвейера. Итоговое сравнение
полного пути — после Phase 4/5.

### Память и запуск (сопоставимо уже сейчас)

| Метрика | Go | C | Разница |
|---|---|---|---|
| idle RSS | 15.3 MB | **3.3 MB** | 4.6× меньше |
| потоки | 10 | **1** | — |
| idle CPU | 0.1% | 0.0% | — |
| RSS peak @10k правил | 40.8 MB (udp c100) | **11.7 MB** | 3.5× меньше |

### Транспортная пропускная способность (namespace, rps, медиана)

| ячейка | Go rps | C rps | p50 Go→C (мс) |
|---|---|---|---|
| udp c10 @1k | 20 811 | **41 115** | 0.44 → 0.23 |
| udp c100 @1k | 34 946 | **53 655** | 2.68 → 1.75 |
| tcp c10 @1k | 11 638 | 12 410 | 0.64 → 0.67 |
| tcp c100 @1k | 17 094 | 14 108 | 4.99 → 6.19 |
| udp c10 @10k | 12 497 | **40 231** | 0.73 → 0.23 |
| udp c100 @10k | 17 580 | **53 494** | 5.22 → 1.76 |

Наблюдения:
- UDP: C стабильно быстрее и, в отличие от Go, **не деградирует с ростом
  числа правил** (Go падает 20.8k→12.5k при 1k→10k правил, потому что
  матчит по всем правилам на каждый ответ; C пока не матчит — деградация
  вернётся частично в Phase 4, задача индексов — удержать её низкой).
- CPU под нагрузкой у C заметно ниже (60–90% vs 140–340% у Go на 4 vCPU),
  что ожидаемо для single-thread event loop против goroutine-на-запрос.
- TCP: C сопоставим на c10 и чуть медленнее на c100 — TCP-путь
  доминируется установкой соединения (один запрос на соединение); это
  зона для профилирования, не блокер.

Raw: `docs/c-rewrite/baseline-raw-c/` (results.jsonl, environment.txt).

## Отличия от Go, зафиксированные в этой фазе

- Malformed **client**-запрос дропается (парсинг как в Go, где хуки
  выставлены). Транспорт форвардит сырые байты валидного запроса.
- Header-only leniency miekg воспроизведена в парсере (см. выше) — ранее
  тест `rejects_malformed` был обновлён под это поведение.
- Упаковщик не компрессирует — сравнение семантическое (спец §21), в
  differential доказано, что канонический разбор совпадает с miekg.
- Прокси-транспорт проверяется running-daemon smoke + ASan под нагрузкой
  + differential парсера, а не unit-тестами (нужны реальные сокеты).

## Нерешённое / перенесённое

- Rule matching / кэш / ipset в hot-path — Phase 4/5 (сейчас callback
  только логирует).
- Дуалстек IPv6 в контейнере не тестируется (нет IPv6); код пути v6
  написан, проверка — на устройстве/в netns (Phase 5).
- 100k-правил и высокая concurrency (1000) на shared-хосте не снимались
  (шум); на C это осмысленно после Phase 4.

## Вход Phase 4

DNS processing: A/AAAA/CNAME/PTR обработка, records cache (bounded,
eviction, TTL, reverse-alias BFS), rule matching в hot-path через
immutable snapshot (RCU-light, D-12), подключение callback прокси к
кэшу+матчингу, differential кэша/обработки, soak на рост памяти.
