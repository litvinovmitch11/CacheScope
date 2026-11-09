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
| Runtime | trace limit | `total`, `recorded`, `truncated` |
| Runtime | explicit reset/flush и `atexit` | поддержано |
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
| rank != 2 на semantic stage | текущий ABI хранит два индекса |
| non-strided memref | нельзя вычислить текущую нормализованную формулу |
| `i1` | не определён byte-level access size |

Отказ является частью контракта полноты, а не случайным ограничением parser.

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
- source location table в JSON;
- перенос access identity через cloning/fusion/unrolling;
- cache simulator и UI.

## Известные ограничения точности

- fallback process addresses зависят от ASLR и allocator;
- semantic access ID пока основан на IR ordinal;
- runtime event ordering определён только для одного потока;
- trace limit отбрасывает хвост, но всегда выставляет `truncated`;
- allocation/deallocation не считаются data accesses;
- разрешённые allocator calls не трассируют внутреннюю работу allocator.
