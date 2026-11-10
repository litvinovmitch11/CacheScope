# Support matrix

## Полностью поддержано

| Уровень | Операция/свойство | Поведение |
|---|---|---|
| MemRef | `memref.load` | semantic read event |
| MemRef | `memref.store` | semantic write event |
| MemRef | rank-2 strided layout | offset/stride normalization |
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
| Frontend | static `16×16 f32` PyTorch matmul | end-to-end demo |
| Transform | tiling и loop interchange | проверено на примерах |

## Обнаруживается, но отклоняется

| Операция/ситуация | Причина |
|---|---|
| `llvm.memcpy/memmove/memset` | нет policy для двух адресов и dynamic size |
| masked load/store | не определено представление mask в trace |
| gather/scatter | один op соответствует нескольким адресам |
| неизвестный direct call | callee может скрывать memory accesses |
| indirect call | нет доказательства memory effects |
| scalable vector access | размер неизвестен compile-time |
| rank != 2 на semantic stage | pass пока реализует только rank-2 normalization |
| non-strided memref | нельзя вычислить текущую нормализованную формулу |
| `i1` | не определён byte-level access size |

Отказ является частью контракта полноты, а не случайным ограничением parser.

## Контракт готов, producer implementation ожидается

| Возможность | Состояние |
|---|---|
| `cachescope.source-map.v1` | schema, validator, fixture и UI navigation rules |
| Stage-aware MLIR dumps | контракт определён, locations ещё не публикуются producer-ом |
| Cross-variant `origin_id` | контракт определён, propagation ещё не реализован |

## Пока не поддерживается

- `memref.copy` как semantic operation;
- semantic atomics;
- `vector.load/store/transfer_read/transfer_write` с vector provenance;
- dynamic tensor shapes в PyTorch demo;
- произвольные rank и element types frontend wrapper;
- межпроцедурный tracing;
- recursion;
- многопоточность;
- GPU/NVVM/ROCDL memory spaces;
- перенос access identity через cloning/fusion/unrolling;
- реализация cache hierarchy и UI (раннер готов, модель передаётся извне).

## Trace runner

Поддерживает полные `cachescope.trace.v1` с `memref_offset` и `read/write`,
автоматическое размещение буферов, локальную статистику каждого уровня и
инструкции, подробный событийный JSON. Обрабатывает файл потоково без лимита
числа событий. Модель реализует `ICacheSimulator`, включая `level_names()` и
`LineAccessResult.level`; деление запросов на строки выполняет модель.

`process_address`, `read_write`, truncated traces и арифметическое переполнение
отклоняются. Подробнее: [runner](../runner/README.md) и
[statistics format](STATISTICS_FORMAT.md).

## Известные ограничения точности

- fallback process addresses зависят от ASLR и allocator;
- semantic access ID пока основан на IR ordinal;
- runtime event ordering определён только для одного потока;
- trace limit отбрасывает хвост, но всегда выставляет `truncated`;
- source location описывает post-transform site, а не cross-variant origin;
- allocation/deallocation не считаются data accesses;
- разрешённые allocator calls не трассируют внутреннюю работу allocator.
