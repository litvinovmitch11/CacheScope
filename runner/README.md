# Trace runner

C++17-библиотека воспроизводит полный `cachescope.trace.v1` через переданный
`CacheHierarchy`. Иерархия владеет уровнями `ICacheLevel`, разбивает обращения
на строки и выполняет exclusive-переносы с сохранением dirty. Раннер назначает
адреса буферам, собирает статистику и записывает отчёт для UI.

Публичный интерфейс находится в `include/cache_simulator/trace_runner.hpp`,
реализация — в `lib/`, тесты и их ожидаемые отчёты — в `tests/`.
Общие trace-fixtures берутся из `schemas/fixtures/`. Уровни кеша и иерархия
реализованы в `simulator/`.

## Сборка без LLVM

```bash
cmake -S runner -B build/runner -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/runner
ctest --test-dir build/runner --output-on-failure
```

Сборка включает `CacheScope::Hierarchy`, настоящий `cachescope_simulator` и их
тесты. CMake скачивает nlohmann/json 3.12.0 и GoogleTest 1.15.2 (при
`BUILD_TESTING=ON`) с проверкой SHA-256. Для сборки без сети укажите
`-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/path/to/json-3.12.0` и
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest-1.15.2`.
Подключение: `add_subdirectory(path/to/runner runner-build)` и
`target_link_libraries(your_target PRIVATE CacheScope::Runner)`.

## CLI

Сборка создаёт `build/runner/cachescope-runner`. Он оборачивает тот же
`run_trace` в configurable exclusive hierarchy с LRU/FIFO/Random,
write-back и write-allocate. Один L1 задаётся так:

```bash
build/runner/cachescope-runner \
  --trace output/trace-demo/baseline/trace.json \
  --output-dir output/trace-demo/simulation/baseline-l1 \
  --cache-name L1 \
  --size-bytes 32768 \
  --line-size-bytes 64 \
  --associativity 8 \
  --replacement-policy lru
```

Для L1/L2/L3 повторите `--cache-level` в порядке от ближайшего к CPU
до самого дальнего:

```bash
build/runner/cachescope-runner \
  --trace output/trace-demo/baseline/trace.json \
  --output-dir output/trace-demo/simulation/baseline-l1-l2-l3 \
  --cache-level L1:32768:64:8:lru \
  --cache-level L2:262144:64:8:lru \
  --cache-level L3:8388608:64:16:random:42
```

Формат уровня: `NAME:SIZE:LINE:WAYS:POLICY[:SEED]`. Seed разрешён только
для `random`. Все уровни exclusive hierarchy должны иметь одинаковый
line size. При наличии `--cache-level` single-level options не используются.

Команда создаёт `statistics.json` и `events.json` в output directory. Размеры
должны образовывать корректную set-associative geometry; подробная диагностика
возвращается до replay. CLI предназначен для готового experiment pipeline.
Random принимает воспроизводимый `--random-seed`, по умолчанию 42. Готовые
команды для сравнения геометрий и policies приведены в
[инструкции по cache models](../docs/CACHE_MODELS.md).

## Интерактивный HTTP backend

Сборка выше создаёт также `build/runner/cachescope_runner_server`.
Запуск на `127.0.0.1:9055`:

```bash
build/runner/cachescope_runner_server
```

Для frontend, запущенного отдельно, разрешите его точный origin:

```bash
build/runner/cachescope_runner_server --allow-origin http://localhost:5173
```

`--port` меняет порт; `--port 0` выбирает свободный порт для тестов.
Адрес сервера выводится в stdout после привязки сокета. Трасса задаётся
абсолютным путём в файловой системе backend:

```bash
curl -s http://127.0.0.1:9055/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"trace_path":"/absolute/path/trace.json","levels":[{"name":"L1","size_bytes":32768,"line_size_bytes":64,"associativity":8,"replacement_policy":"LRU"},{"name":"L2","size_bytes":262144,"line_size_bytes":64,"associativity":8,"replacement_policy":"FIFO"}]}'
```

POST возвращает 202 с `session_id` и `status=preparing`. Дождитесь ready или
finished через GET `/api/v1/sessions/<id>`, затем используйте revision
из ответа:

```bash
curl -s 'http://127.0.0.1:9055/api/v1/sessions/<id>/step' \
  -H 'Content-Type: application/json' \
  -d '{"expected_revision":"1","count":10}'
curl -s 'http://127.0.0.1:9055/api/v1/sessions/<id>/state?expected_revision=2&level=1&set_offset=0&set_limit=16'
curl -s 'http://127.0.0.1:9055/api/v1/sessions/<id>/instructions?expected_revision=2&offset=0&limit=100'
```

Полный [HTTP-контракт](../docs/RUNNER_API.md) описывает все девять операций,
reset/delete, ошибки и порядок восстановления после потери ответа. Backend
поддерживает LRU/FIFO/Random, exclusive/write-back/write-allocate, точные
64-битные строковые DTO, физические страницы slots и накопительную статистику.
Встроенные лимиты — 1000 событий на step, 64 sets и 100 инструкций на страницу;
клиенты читают актуальные лимиты из Session. API кэшей и файловые отчёты не меняются.

Подготовка асинхронна и отменяется при DELETE. Валидированные события пишутся
во временный дисковый spool, а не в массив всей трассы. Один фоновый worker
готовит сессию; HTTP workers обслуживают step/reset синхронно, блокируя другие
мутации и snapshot-reads через session_busy. GET статуса остаётся доступным.
Revision публикуется только на границах команд. Исходный trace должен оставаться
неизменным; проверяются inode, device, размер, mtime и ctime до/после исполнения.
Временный spool удаляется при освобождении сессии/завершении процесса.

Снимки получает wrapper над ICacheLevel: он наблюдает реальные insert,
invalidate, touch и clear каждого уровня, включая внутренние переносы.
Он сохраняет set/way/dirty в объёме конфигурации кеша и не восстанавливает
состояние из неполного demand-журнала. Интерфейс ICacheLevel остаётся прежним.

Программный пошаговый replay доступен в `trace_replay.hpp`: `TraceReplay`
принимает trace и существующую иерархию, выполняет preflight и предоставляет
step/reset, metadata и статистику. Caller сериализует вызовы. После ошибки
исполнения требуется reset; TraceChanged требует заново создать replay.
Этот C++ API возвращает числовые artifact DTO, HTTP-слой отдельно кодирует
64-битные значения строками.

HTTP transport — закреплённый cpp-httplib 0.66.0 с SHA-256, без TLS и compression.
Для offline configure задайте `-DFETCHCONTENT_SOURCE_DIR_HTTPLIB=/path/to/cpp-httplib-0.66.0`.
Только библиотека без HTTP-зависимости:

```bash
cmake -S runner -B build/runner-library -DCACHESCOPE_BUILD_HTTP_SERVER=OFF -DBUILD_TESTING=OFF
```

HTTP и восстановление после частичной ошибки модели проверяются без LLVM/Node:

```bash
ctest --test-dir build/runner -R 'runner-(http|session-failure)$' --output-on-failure
```

## Batch API

```cpp
#include <cache_simulator/trace_runner.hpp>
#include <cache_simulator/LRU.h>
#include <cache_simulator/SetAssociativeCacheLevel.h>
#include <utility>

cache_simulator::RunSummary replay() {
  auto l1 = std::make_unique<cache_simulator::SetAssociativeCacheLevel>(
      cache_simulator::CacheLevelConfig{32768, 64, 8},
      [] { return std::make_unique<cache_simulator::LRU>(); });
  std::vector<cache_simulator::CacheLevel> levels;
  levels.push_back({"L1", {32768, 64, 8, "LRU"}, std::move(l1)});
  cache_simulator::CacheHierarchy hierarchy(std::move(levels));
  return cache_simulator::run_trace(
      "trace.json", hierarchy, "output/simulation/demo/statistics.json");
}
```

Конфигурация должна соответствовать реальному уровню. Уровни реализуют
`lookup/touch/invalidate/insert/clear`; иерархия получает исключительное владение ими.
Используется актуальный `ICacheLevel` из `CacheLevel.h`.
Подробнее: [CacheHierarchy](../simulator/README.md).

Перед запуском раннер проверяет весь файл, автоматически назначает буферам
непересекающиеся адреса и вызывает `hierarchy.clear()`. Вторым проходом он
вызывает `hierarchy.read/write` и пишет `events.json` рядом с указанным файлом
сводки. Имена этих двух файлов должны различаться. Для каждого эксперимента,
конфигурации и варианта нужна собственная директория результатов.

Авторазмещение идёт по возрастанию memref_id от `0x10000000`, базы выравниваются
на 64 байта. Диапазоны включают смещение 0; отрицательные смещения сохраняются.
Размещение записывается в отчёте. При сравнении вариантов нужно проверять
совпадение размещений, поскольку они влияют на конфликты кеша.

Вход должен оставаться неизменным на протяжении обоих проходов. Число событий
не ограничено; память зависит от static metadata, числа инструкций, буферов,
уровней и размера одного запроса. Адреса и счётчики имеют разрядность uint64_t.

Поддержаны полные semantic-трассы `memref_offset` с `read/write`: `valid=true`,
`complete=true`, `truncated=false`, `errors=[]`. Поля объекта `producer`
проверяются по актуальному trace v1; `indices` не требуется и игнорируется,
как и прочие дополнительные поля event. `source_location`, optional
`origin_ids`/`stage_locations` переносятся в `instructions`, embedded
`source_map` — в `source`. Раннер не разрешает пути артефактов source map;
проверку его ссылок и fingerprint выполняет `scripts/validate_trace.py`.
`process_address`, `read_write`, truncated traces, нарушение schema, типов,
последовательности и переполнение адресов отклоняются до очистки уровней.
Ошибка исполнения оставляет состояние иерархии частично изменённым; новый
независимый запуск снова начинает с clear. Оба файла отчёта сохраняются при
ошибке модели или записи; ограничения публикации описаны в контракте.

## Отчёт и тесты

Формат из [PR #2](https://github.com/litvinovmitch11/CacheScope/pull/2):
`cachescope.simulation-statistics.v1` + `cachescope.simulation-events.v1`.
Базовые поля описывают L1; расширения содержат остальные уровни, инструкции,
dirty evictions и память. [Контракт и fixtures](../docs/STATISTICS_FORMAT.md).

Suites раннера и иерархии включают hierarchy, integration, real-cache,
fifo-cache, random-cache, validation, random-miss, streaming и CLI smoke для
LRU/FIFO/Random и L1/L2/L3; также запускаются upstream-тесты симулятора.
Hierarchy и real-cache используют настоящий SetAssociativeCacheLevel с LRU.
Real-cache проверяет и канонические compiler fixtures, включая source mapping.
Fifo-cache и random-cache прогоняют короткую трассу через настоящие уровни
SetAssociativeCacheLevel и двухуровневую иерархию. FIFO проверяет порядок
вытеснения, продвижение из L2 и dirty writeback. Random использует seed 42 и
проверяет повторяемость JSON-отчётов после очистки иерархии.

```bash
ctest --test-dir build/runner -R 'runner-(fifo-cache|random-cache)$' --output-on-failure
```

Потоковый тест обрабатывает 1 000 001 event
и проверяет, что сводка остаётся маленькой. Smoke использует отдельный тестовый
`ICacheLevel` с независимой вероятностью miss 50%, seed 42; точные 50% в выборке
не требуются.

```bash
ctest --test-dir build/runner -R random-miss -V
```

Smoke сохраняет `trace.json`, `statistics.json`, `events.json` в подкаталоге
`smoke/` каталога сборки. При следующем успешном запуске отчёт заменяется.
