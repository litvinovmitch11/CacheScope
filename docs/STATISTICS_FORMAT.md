# Отчёты runner и CacheHierarchy

Базовый контракт взят из [PR #2](https://github.com/litvinovmitch11/CacheScope/pull/2):
[SIMULATION_FORMAT.md](SIMULATION_FORMAT.md). Раннер создаёт два файла:

- `statistics.json`: `cachescope.simulation-statistics.v1`, компактная сводка;
- `events.json`: `cachescope.simulation-events.v1`, подробный событийный ряд.

Файл событий записывается потоково. Сводка не содержит массива events и не
растёт с числом динамических событий при неизменном наборе access_id.
Примеры: [trace](../runner/tests/fixtures/example.trace.json),
[statistics](../runner/tests/fixtures/example.statistics.json),
[events](../runner/tests/fixtures/example.events.json).
Fixture использует заданные исходы тестовых уровней, а не реальные CPU counters.

## Совместимость с UI

Контракт [SIMULATION_FORMAT.md](SIMULATION_FORMAT.md), включая расширение
из [PR #5](https://github.com/litvinovmitch11/CacheScope/pull/5), описывает уровни кеша.
Базовые поля сводки `cache_config`,
`processed`, `totals` и поле `lines` каждого события относятся к **L1**.
Они сохраняют смысл и типы контракта PR. В частности, `way` L1 известен после
hit или заполнения, а все адреса сериализуются hex-строками, включая адрес 0.

В `cache_config` записываются фактические значения, переданные при создании
иерархии: size, line size, associativity, computed set count, replacement policy.
Поля уровней соответствуют общему контракту UI; дополнительные метаданные ниже
прежний UI может игнорировать. Нельзя суммировать hits/misses разных уровней
для вычисления локального hit rate.

## Расширения statistics.json

| Поле | Смысл |
|---|---|
| `levels` | `{level, name, cache_config, processed, totals}` для каждого уровня |
| `instructions` | статистика по числовому `access_id`, отсортированная по ID |
| `buffers` | модельные базы массивов и зарезервированные диапазоны смещений |
| `source` | header исходной трассы, полный объект producer и optional source_map |
| `hierarchy` | политики exclusive, write_back, write_allocate |
| `requests` | число исходных events, reads/writes и запрошенные байты |
| `memory` | число line reads из памяти и dirty writebacks в память |

Корневой `producer` остаётся строкой `producer.name` исходной трассы, как
требует UI. Полный объект с версиями и pipeline fingerprint доступен в
`source.producer`.

Каждая инструкция сохраняет поля static access table, включая `source_location`,
`address_kind` и optional `origin_ids`/`stage_locations`. Она также содержит `events`,
`requested_bytes` и массив `levels`. Пути stage artifacts в `source.source_map`
остаются относительными к исходному trace bundle; для навигации UI нужен этот
bundle. Записи уровня содержат `level`,
`processed`, `totals`. В `processed.trace_events` уровня входят только события,
реально проверявшие этот уровень. `line_accesses` считает отдельные demand probes.
Read/write counters классифицируются по исходному `event.kind`, а не по
внутреннему чтению строки для write-allocate или служебным переносам.

В `totals` дополнительно есть `writebacks` (dirty victims данного уровня,
включая переносы ниже) и `hit_rate_percent = 100 * hits / line_accesses`.
При нуле probes процент равен `null`. Отдельно `memory.writebacks` считает только
записи в память после вытеснения из последнего уровня. Финального flush нет.

Вытеснения считаются по фактическим insert victims, включая каскады. Служебные
переносы не считаются дополнительными demand hits/misses. Все их счётчики
приписываются инструкции, которая вызвала перенос.

В `buffers` сохранены `memref_id`, hex `base_address`, `min_offset` и
`end_offset`. Полуинтервалы включают логическое смещение 0. Модельный адрес:
`base_address + byte_offset`, в том числе для отрицательных смещений.

## Расширения events.json

Заголовок содержит массив `levels` с именами и конфигурациями. Событие содержит
обязательные поля PR (`sequence`, `kind`, `address`, `size_bytes`, `lines`) и:

- `access_id` и `trace_event`: связь с инструкцией и полный исходный event;
- `level_results`: `{level, lines}` для каждого реально посещённого уровня;
- `evictions`: все `{level, line_address, dirty}` при выполнении события;
- `memory_reads`, `memory_writebacks`: обращения в память для этого события.

В строках `level_results` есть дополнительный `evicted_dirty` или `null`.
У нижнего уровня `way` может быть `null`: exclusive miss не размещает там
запрошенную строку. В базовом `lines` L1 way всегда определён.

Ось событийного ряда — `sequence`, а не время в секундах. UI может рассчитать
оконные показатели по каждому уровню и access_id без повторной симуляции.

## Публикация

Оба файла сначала создаются рядом с назначением как временные. После успешного
прогона и закрытия файлов заменяется пара; ошибки обычной публикации откатываются
через резервные имена. Это не crash-atomic транзакция. Один bundle нельзя
одновременно читать или перезаписывать во время публикации.

Прежний экспериментальный `cachescope.statistics.v1` с events внутри сводки
больше не формируется. Новый контракт использует названия схем из PR #2.
