# Командная интеграция

В проекте четыре человека и три независимых компонента. Главная задача
интеграции — не заставлять разработчика cache emulator или UI собирать LLVM и
не связывать их код с внутренним MLIR IR.

```text
1. Compiler trace producer
   PyTorch/MLIR + inputs → native execution → trace bundle
                                         ↓
2–3. Cache emulator      trace + cache config → statistics
                                         ↓
4. UI                    inputs + trace + statistics → visualization
```

## 1. LLVM/MLIR/PyTorch и trace producer

Первый разработчик владеет всей compiler-вертикалью:

- PyTorch export и входными тензорами;
- MLIR transformations и lowering;
- MemRef/LLVM instrumentation;
- compiler runtime, который записывает trace;
- numerical validation;
- версией trace schema.

Его внешний результат — данные в `output/<experiment>/`, а не MLIR plugin или
внутренние runtime calls. Канонический compiler experiment выглядит так:

```text
output/trace-demo/
  summary.json               self-check compiler variants
  baseline/trace.json        baseline trace
  tile4/trace.json           tiled trace
  tile4-ikj/trace.json       tiled + interchanged trace
```

Payload и transform programs хранятся в Git под `examples/`. Cache emulator
для replay использует только `trace.json` и собственную cache config. PyTorch
integration дополнительно сохраняет точные tensors и input manifest, но это не
обязательная часть базового trace contract.

Compiler-разработчик гарантирует:

1. вычисленный результат совпадает с PyTorch oracle;
2. trace либо полон в рамках support matrix, либо compilation завершается
   ошибкой;
3. `truncated` не скрывается;
4. consumer проверяет `valid` и `complete` до replay;
5. static access table не зависит от записанного event prefix;
6. schema меняется только с новой версией и compatibility test;
7. generated IR и binaries не являются командным API.

Его обязательная проверка:

```bash
scripts/check.sh --with-lint
python3 examples/trace_demo.py
```

Изменения optional PyTorch frontend дополнительно требуют
`scripts/check.sh --with-torch`.

## 2–3. Cache emulator

Второй и третий разработчики совместно владеют emulator. Их код не должен
зависеть от LLVM, MLIR, torch-mlir или `compiler/runtime`.

Вход emulator:

- один `cachescope.trace.v1` JSON;
- cache configuration: line size, capacity, associativity, replacement и
  write policy.

Для semantic event адрес модели вычисляется так:

```text
model_address = deterministic_base[memref_id] + byte_offset
```

Для `process_address` emulator должен либо поддерживать отдельную policy, либо
явно отклонять такой trace. Абсолютные fallback-адреса нельзя сравнивать между
разными процессами.

Emulator обязан:

- проверить `schema` до replay;
- отклонить `valid = false` и `complete = false` до replay;
- назначить непересекающиеся deterministic bases;
- разбить доступ, пересекающий cache-line boundary;
- вернуть детерминированные statistics для одинаковых trace/config;
- не использовать `source_location` как memory address или origin identity.

Реализованы [runner](../runner/README.md) и [CacheHierarchy](../hierarchy/README.md).
Раннер получает иерархию по ссылке, проверяет trace, очищает уровни и вызывает
`read/write(address, size)`. Иерархия владеет уровнями `ICacheLevel` и выполняет
exclusive-переносы; SetAssociativeCacheLevel и LRU находятся в `simulator/`.

Результат runner:

```text
output/<experiment>/simulation/<config-name>/
  statistics.json            totals: hits, misses, evictions, writebacks
  events.json                per-access cache outcome, записывается потоково
```

Используются `cachescope.simulation-statistics.v1` и
`cachescope.simulation-events.v1` из [контракта UI](SIMULATION_FORMAT.md).
Базовые поля проецируют L1, `levels` и `level_results` описывают остальные
уровни; [расширения раннера](STATISTICS_FORMAT.md) связывают их с инструкциями.
Полный объект producer и source mapping сохраняются. Поддержаны `memref_offset`
и `read/write`; fallback, atomics и неполные трассы явно отклоняются. Первый
разработчик не должен добавлять cache outcomes в compiler runtime.

## 4. UI

Четвёртый разработчик владеет только представлением и orchestration UI. UI не
компилирует MLIR и не реализует cache policy.

UI получает:

- `input.json` и при необходимости `inputs.npz` от trace producer;
- `trace.json` от trace producer;
- stage dumps и embedded `source_map` от канонического producer-а;
- `statistics.json` и опциональный `events.json` от emulator.

UI может показывать порядок обращений, offsets, read/write kind и формальный
результат cache model. Он не должен делать выводы о wall-clock performance или
CPU cycles только по trace.

Stage-aware navigation определена в [SOURCE_MAPPING.md](SOURCE_MAPPING.md).
UI обязан feature-detect поле `source_map` для совместимости со старыми traces
и не пытаться разобрать legacy `source_location` как стабильную структуру.

На первом этапе UI разумно поддержать импорт готовой директории experiment.
Запуск compiler pipeline из UI лучше добавлять только после стабилизации bundle
manifest и отдельного CLI/API orchestration слоя.

## Контракты между участниками

| Producer | Consumer | Контракт | Владелец версии |
|---|---|---|---|
| Compiler | Emulator | `cachescope.trace.v1` | участник 1 |
| Compiler | UI | `cachescope.input.v1`, tensors | участник 1 |
| Emulator | UI | `cachescope.simulation-statistics.v1`, `cachescope.simulation-events.v1` | участники 2–3 |
| Все компоненты | Все | experiment directory layout | совместно |

Изменение общего контракта делается отдельным commit и включает fixture для
consumer. Не следует импортировать исходники соседнего компонента ради обхода
схемы.

## Что считать интеграционным fixture

В Git стоит хранить маленькие trace/input/statistics fixtures. Полные результаты
локальных запусков остаются в игнорируемом `output/`. Fixture должен быть
достаточно маленьким для code review и иметь явно указанную schema version.

Текущий trace contract подробно описан в [TRACE_FORMAT.md](TRACE_FORMAT.md), а
поддерживаемые compiler operations — в [SUPPORT_MATRIX.md](SUPPORT_MATRIX.md).
Перед передачей fixture consumer-команде запускается:

```bash
python3 scripts/validate_trace.py path/to/trace.json
```
