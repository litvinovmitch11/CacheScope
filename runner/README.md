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

## API

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

Шесть suites раннера и иерархии: hierarchy, integration, real-cache, validation,
random-miss и streaming; также запускаются upstream-тесты симулятора.
Hierarchy и real-cache используют настоящий SetAssociativeCacheLevel с LRU.
Real-cache проверяет и канонические compiler fixtures, включая source mapping.
Потоковый тест обрабатывает 1 000 001 event
и проверяет, что сводка остаётся маленькой. Smoke использует отдельный тестовый
`ICacheLevel` с независимой вероятностью miss 50%, seed 42; точные 50% в выборке
не требуются.

```bash
ctest --test-dir build/runner -R random-miss -V
```

Smoke сохраняет `trace.json`, `statistics.json`, `events.json` в подкаталоге
`smoke/` каталога сборки. При следующем успешном запуске отчёт заменяется.
