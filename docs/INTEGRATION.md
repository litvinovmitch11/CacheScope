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
внутренние runtime calls. Для Torch-демо bundle выглядит так:

```text
output/torch-demo/
  input.json                 воспроизводимое описание входа
  inputs.npz                 точные входные tensors
  comparison.json            self-check compiler variants
  baseline/trace.json        baseline trace
  tile4/trace.json           tiled trace
  tile4-ikj/trace.json       tiled + interchanged trace
```

`input.json` и `inputs.npz` нужны для воспроизведения эксперимента и UI. Cache
emulator для replay использует только `trace.json` и собственную cache config.

Compiler-разработчик гарантирует:

1. вычисленный результат совпадает с PyTorch oracle;
2. trace либо полон в рамках support matrix, либо compilation завершается
   ошибкой;
3. `truncated` не скрывается;
4. schema меняется только с новой версией и compatibility test;
5. generated IR и binaries не являются командным API.

Его обязательная проверка:

```bash
scripts/check.sh --with-torch --with-lint
```

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
- отклонить или явно отметить `truncated = true`;
- назначить непересекающиеся deterministic bases;
- разбить доступ, пересекающий cache-line boundary;
- вернуть детерминированные statistics для одинаковых trace/config;
- не интерпретировать MLIR indices как физический адрес.

В emulator уже есть [runner](../runner/README.md), который принимает готовый
`ICacheSimulator`; сама модель и её конфигурация реализуются отдельно.
Раннер вызывает `clear()`, затем `read/write`. Модель сообщает названия уровней
через `level_names()` и возвращает demand probes всех посещённых уровней
с 1-based `LineAccessResult.level`. Разбиение запроса на строки выполняет модель.

Раннер v1 принимает полные semantic-трассы `memref_offset` с `read/write`;
`process_address`, `read_write` и truncated traces отклоняются. Адреса массивов
назначаются автоматически и сохраняются в отчёте; YAML buffer names не
используются для сопоставления с числовыми memref_id.

Результат runner:

```text
output/<experiment>/simulation/<config-name>/
  statistics.json            totals + instructions + every event/level outcome
```

Контракт [cachescope.statistics.v1](STATISTICS_FORMAT.md) версионируется
отдельно от trace. Он содержит hits, misses и evictions; writebacks пока
не наблюдаемы через интерфейс модели. Первый разработчик не должен добавлять
cache outcomes в compiler runtime.

## 4. UI

Четвёртый разработчик владеет только представлением и orchestration UI. UI не
компилирует MLIR и не реализует cache policy.

UI получает:

- `input.json` и при необходимости `inputs.npz` от trace producer;
- `trace.json` от trace producer;
- `statistics.json`, включая подробный массив events, от runner.

UI может показывать порядок обращений, offsets, read/write kind и формальный
результат cache model. Он не должен делать выводы о wall-clock performance или
CPU cycles только по trace.

На первом этапе UI разумно поддержать импорт готовой директории experiment.
Запуск compiler pipeline из UI лучше добавлять только после стабилизации bundle
manifest и отдельного CLI/API orchestration слоя.

## Контракты между участниками

| Producer | Consumer | Контракт | Владелец версии |
|---|---|---|---|
| Compiler | Emulator | `cachescope.trace.v1` | участник 1 |
| Compiler | UI | `cachescope.input.v1`, tensors | участник 1 |
| Emulator | UI | `cachescope.statistics.v1` | участники 2–3 |
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
