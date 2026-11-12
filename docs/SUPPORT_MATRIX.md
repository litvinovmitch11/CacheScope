# Support matrix

## Полностью поддержано

| Уровень | Операция/свойство | Поведение |
|---|---|---|
| MemRef | `memref.load` | semantic read event |
| MemRef | `memref.store` | semantic write event |
| MemRef | arbitrary-rank strided layout | offset/stride normalization |
| MemRef | scalar int/float, кроме `i1` | byte-addressable size |
| Roots | function arguments | ID равен номеру аргумента |
| Roots | `memref.alloc`, `memref.alloca` | последовательный root ID |
| Views | `ViewLikeOpInterface`, включая subview | root ID сохраняется |
| LLVM | unmarked `llvm.load/store` | pointer fallback event |
| LLVM | unmarked `llvm.atomicrmw/cmpxchg` | `read_write` fallback event |
| Manifest | static access table | все sites регистрируются до выполнения kernel body |
| Manifest | source location | строковая MLIR location для каждого site |
| Runtime | trace status | `valid`, `complete`, `errors`, truncation invariants |
| Runtime | trace limit | `total`, `recorded`, `truncated` |
| Schema | `cachescope.trace.v1` | JSON Schema, validator и golden fixtures |
| Runtime | explicit reset/flush и `atexit` | поддержано |
| Example | three MLIR compiler variants | canonical end-to-end driver |
| Product E2E | compiler → simulator → standalone UI | одна команда и переносимый experiment bundle |
| Runner CLI | configurable L1/L2/L3 hierarchy | per-level LRU/FIFO/Random/PLRU4, WB/WT, WA/NWA; mixed exclusive/inclusive/NINE |
| UI | statistics comparison и standalone HTML | три variants и cache levels |
| Frontend | static `16×16 f32` PyTorch matmul | end-to-end demo |
| Transform | tiling и loop interchange | проверено на примерах |
| Source map | input → transformed → loops → instrumentation → LLVM | embedded `cachescope.source-map.v1` |
| Provenance | один high-level origin → несколько access sites | проверено на baseline/tile/interchange |
| Torch Studio | пользовательские `model.py`, `inputs.py`, Transform schedules | CPU f32 static shapes, один Tensor output; операции в пределах torch-mlir/current lowering |
| Cache profiles | compact strings и structured `{levels, hierarchy}` | все schedules replay на одинаковых profiles |
| Hardware presets | IntelDX2 L1 WT, Enhanced IntelDX4 L1 WB | документированные PLRU/write/allocation policies |
| Interactive UI | report → выбранные schedule/profile → Stepper | точные levels/hierarchy, event, source location, sets/ways, timeline |
| External validation | Cachegrind, drcachesim, drmemtrace | воспроизводимые scripts и CI workflows |

## Обнаруживается, но отклоняется

| Операция/ситуация | Причина |
|---|---|
| `llvm.memcpy/memmove/memset` | нет policy для двух адресов и dynamic size |
| masked load/store | не определено представление mask в trace |
| gather/scatter | один op соответствует нескольким адресам |
| неизвестный direct call | callee может скрывать memory accesses |
| indirect call | нет доказательства memory effects |
| scalable vector access | размер неизвестен compile-time |
| non-strided memref | нельзя вычислить текущую нормализованную формулу |
| `i1` | не определён byte-level access size |

Отказ является частью контракта полноты, а не случайным ограничением parser.

## Пока не поддерживается

- `memref.copy` как semantic operation;
- semantic atomics;
- `vector.load/store/transfer_read/transfer_write` с vector provenance;
- dynamic tensor shapes в PyTorch demo;
- произвольные shapes и element types текущего PyTorch frontend wrapper;
- межпроцедурный tracing;
- recursion;
- многопоточность;
- GPU/NVVM/ROCDL memory spaces;
- доказанная корректность provenance для произвольных fusion/unrolling passes;
- обратная навигация из произвольной MLIR operation ко всем dynamic events.

## Trace runner

Независимые C++17-модули `runner/` и `simulator/` работают без LLVM. Раннер
принимает полный valid trace v1 с объектом producer, status flags и static
access table. Поддержаны `memref_offset` и `read/write`, отрицательные смещения,
разбиение на строки, hit/miss по инструкциям и уровням, полный событийный ряд.
`indices` не требуется. Метаданные source mapping переносятся в отчёт для UI.

CacheHierarchy владеет `ICacheLevel` из актуального `CacheLevel.h`, реализует
настраиваемые exclusive/inclusive/NINE, WB/WT и WA/NWA с одинаковым line size всех уровней.
Готовые уровни — SetAssociativeCacheLevel с LRU, FIFO и Random. Нет ограничения
числа событий раннера; память зависит от static sites и размера одного запроса. Неполные,
invalid и truncated traces, `process_address` и `read_write` отклоняются.
Подробнее: [runner](../runner/README.md), [hierarchy](../simulator/README.md) и
[statistics](STATISTICS_FORMAT.md).

## Известные ограничения точности

- fallback process addresses зависят от ASLR и allocator;
- semantic access ID пока основан на IR ordinal;
- runtime event ordering определён только для одного потока;
- trace limit отбрасывает хвост, но всегда выставляет `truncated`;
- legacy `source_location` не является cross-variant identity; для этого
  используется `source_map`;
- allocation/deallocation не считаются data accesses;
- разрешённые allocator calls не трассируют внутреннюю работу allocator.
