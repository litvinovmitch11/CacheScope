# CacheScope roadmap

Этот roadmap отражает состояние репозитория, а не первоначальный хакатонный
замысел. Реализованные возможности перечислены в
[docs/SUPPORT_MATRIX.md](docs/SUPPORT_MATRIX.md).

## Текущая точка

Compiler/runtime вертикаль готова для статического rank-2 `f32` matmul:

```text
PyTorch или MLIR payload
  → Transform Dialect strategy
  → MemRef semantic instrumentation
  → LLVM coverage/fallback instrumentation
  → native execution
  → trace.v1 JSON
```

Численная эквивалентность и различие traces проверяются автоматически.
Канонический pipeline находится в `examples/trace_demo.py`.

Реализованы SetAssociativeCacheLevel с LRU/FIFO/Random, exclusive
`CacheHierarchy` и независимый `runner/`: полные semantic-трассы,
автоматическое размещение буферов,
статистика по уровням/инструкциям и событийный ряд для UI. Тесты используют
реальную модель и отдельную smoke-модель с вероятностью miss 50%.

Reference product flow `compiler → trace → simulator → standalone UI` запускается
одной командой `python3 examples/product-e2e/run.py`. Он создаёт три variants,
simulation reports и переносимый HTML в одной experiment directory.

## M0: контракт trace producer — выполнено

- `cachescope.trace.v1` исправлен до появления внешних consumers;
- logical indices удалены из core event и ABI;
- static access table не зависит от dynamic events и truncation;
- каждый site содержит `source_location` и `address_kind`;
- header содержит producer/toolchain metadata и pipeline fingerprint;
- `valid`, `complete`, `errors` формализуют состояние trace;
- добавлены JSON Schema, validator, golden fixtures и compatibility tests;
- готовый JSON публикуется через временный файл.

## M1: provenance через transformations

- stage-aware `cachescope.source-map.v1` contract, validator и fixture готовы;
- `origin_id` назначается до Transform Dialect pipeline;
- logical origin отделён от конкретного post-transform `access_id`;
- canonical baseline/tiling/interchange pipeline публикует пять stage dumps;
- cross-variant matching и реальные dump coordinates проверяются end-to-end;
- далее: расширить и проверить lineage для fusion/unrolling.

## M2: расширить memory coverage

- добавить semantic policies для `memref.copy` и atomics;
- нормализовать или поддержать `vector.load/store/transfer_*`;
- добавить LLVM handlers для memcpy/memmove/memset;
- определить поведение masked load/store и gather/scatter;

## M3: укрепить runtime и driver

- сделать runtime thread-safe либо явно отклонять многопоточные запуски;
- выделить единый compiler driver вместо дублирования pipeline в demos;
- добавить installed-plugin smoke test и CI.

## Общие продуктовые этапы

## P1: cache simulator — выполнено

- детерминированные model base addresses для semantic memrefs;
- configurable L1/L2/L3 hierarchy: per-level size, line size, associativity
  и replacement policy;
- LRU/FIFO/Random, write-back и write-allocate;
- split accesses через границу cache line;
- hit/miss, eviction и writeback events;
- replay state для каждого set/way;
- одинаковый результат повторных запусков.

## P2: сравнение вариантов — частично

- реализованы aggregate hit/miss comparison и переносимый run bundle;
- далее: сопоставление events по provenance, а не sequence;
- далее: matched/unmatched coverage и reuse distance;
- далее: cache outcome diff;
- далее: отдельная обработка fallback events, адреса которых зависят от процесса.

## P3: визуализация — частично

- реализованы statistics comparison и экспорт воспроизводимого HTML-отчёта;
- далее: payload и transform IR рядом;
- далее: event timeline и состояние cache sets/ways;
- далее: синхронный baseline/transformed replay;
- далее: переход из события к исходной MLIR operation.

## Не входит в ближайшие этапы

- cycle-accurate CPU simulation;
- wall-clock performance prediction;
- PMU correlation;
- coherence и многопоточная cache hierarchy;
- GPU memory hierarchy;
- автоматическая генерация optimization advice;
- autotuning transform schedules.

## Definition of Done следующего compiler milestone

Пункты 2–5 уже выполнены текущим product E2E. Оставшийся gate следующего
compiler milestone — расширение покрытия memory operations:

1. все memory operations выбранного CPU kernel либо инструментируются, либо
   останавливают compilation с точной диагностикой;
2. trace schema покрыта compatibility tests;
3. simulator воспроизводимо обрабатывает semantic trace;
4. baseline и tiled variant дают проверяемый cache diff;
5. новый разработчик воспроизводит результат одной командой.
