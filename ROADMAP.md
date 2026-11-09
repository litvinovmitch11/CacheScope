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

## P0: завершить контракт trace producer

- добавить semantic policies для `memref.copy` и atomics;
- нормализовать или поддержать `vector.load/store/transfer_*`;
- добавить LLVM handlers для memcpy/memmove/memset;
- определить поведение masked load/store и gather/scatter;
- перенести provenance через transformations, которые клонируют access sites;
- добавить source-location table в JSON;
- сделать runtime thread-safe либо явно отклонять многопоточные запуски;
- формализовать schema versioning и compatibility tests.

## P1: cache simulator

- детерминированные model base addresses для semantic memrefs;
- configurable L1: size, line size, associativity;
- LRU, write-back и write-allocate;
- split accesses через границу cache line;
- hit/miss, eviction и writeback events;
- replay state для каждого set/way;
- одинаковый результат повторных запусков.

## P2: сравнение вариантов

- сопоставление events по provenance, а не sequence;
- matched/unmatched coverage;
- reuse distance;
- cache outcome diff;
- отдельная обработка fallback events, адреса которых зависят от процесса;
- компактный переносимый run bundle.

## P3: визуализация

- payload и transform IR рядом;
- event timeline;
- состояние cache sets/ways;
- синхронный baseline/transformed replay;
- переход из события к исходной MLIR operation;
- экспорт воспроизводимого отчёта.

## Не входит в ближайшие этапы

- cycle-accurate CPU simulation;
- wall-clock performance prediction;
- PMU correlation;
- coherence и многопоточная cache hierarchy;
- GPU memory hierarchy;
- автоматическая генерация optimization advice;
- autotuning transform schedules.

## Definition of Done следующего milestone

Следующий milestone готов, когда:

1. все memory operations выбранного CPU kernel либо инструментируются, либо
   останавливают compilation с точной диагностикой;
2. trace schema покрыта compatibility tests;
3. simulator воспроизводимо обрабатывает semantic trace;
4. baseline и tiled variant дают проверяемый cache diff;
5. новый разработчик воспроизводит результат одной командой.
