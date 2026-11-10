# Архитектура CacheScope

## 1. Область системы

Текущий CacheScope — trace producer для однопоточных CPU kernels. Он принимает
структурированный MLIR либо PyTorch-модель, применяет контролируемые compiler
transformations, выполняет получившийся код и записывает обращения к памяти.

```text
frontend
  → structured MLIR
  → transformation strategy
  → explicit memory operations
  → hybrid instrumentation
  → LLVM/native execution
  → trace.v1 JSON
```

`runner/` — независимый потребитель trace: C++-библиотека вызывает переданный
`ICacheSimulator` и собирает статистику по инструкциям и уровням. Реализация
самой cache hierarchy и UI пока отсутствуют. Раннер не зависит от LLVM/MLIR;
подключение описано в [runner README](../runner/README.md).

## 2. Компоненты

### `compiler/cs-trace`

Out-of-tree plugin для `mlir-opt`. Регистрирует два module passes:

- `cachescope-instrument-memory`;
- `cachescope-instrument-llvm-fallback`.

Plugin не занимается JSON и файловым вводом-выводом. Его задача — определить
access sites, вычислить аргументы ABI и вставить runtime calls.

### `compiler/runtime`

Небольшая shared library без зависимости от MLIR. Она принимает события через
C ABI, назначает динамический `sequence`, применяет trace limit и сериализует
JSON. Runtime расположен внутри `compiler/`, потому что является последней
частью trace producer. Он не является runtime cache emulator.

### `examples/torch-demo`

Frontend и orchestration layer. Он экспортирует `torch.nn.Module` через
torch-mlir, запускает Transform Dialect, bufferization, instrumentation,
LLVM lowering, AOT compilation и вызов через `ctypes`.

### `tests`

Три независимых уровня:

- pass unit/integration tests;
- runtime ABI/serialization tests;
- полный baseline/tiled MLIR execution test.

## 3. Почему instrumentation гибридная

Одна точка инструментации не даёт одновременно полноту и provenance.

На MemRef-уровне доступны:

- корневой buffer;
- логические индексы;
- strides и offset;
- связь с transformation pipeline.

На LLVM-уровне видны фактические memory instructions, но обычно уже потеряны
memref identity и логические индексы.

Поэтому semantic MemRef stage является основным, а LLVM stage — coverage audit
и fallback.

```text
memref.load/store
  → semantic event + provenance marker
  → LLVM lowering
  → marked llvm.load/store: already covered
  → unmarked LLVM access: fallback event
  → unknown effect/call: compilation error
```

## 4. Semantic MemRef pass

Pass запускается после lowering Linalg в явные `memref.load/store`, но до LLVM
conversion:

```text
builtin.module(
  cachescope-instrument-memory{target-func=kernel},
  canonicalize,
  cse
)
```

Он обходит только `target-func`. Setup, input initialization и result checking
могут оставаться в `main` и не загрязняют эксперимент.

### Buffer identity

- memref arguments получают ID, равный номеру аргумента;
- `memref.alloc/alloca` получают последующие ID в порядке IR;
- `ViewLikeOpInterface` отслеживается назад до корня;
- subview сохраняет ID исходного buffer.

Это детерминировано внутри одного static kernel, но пока не является
межпроцедурным allocation registry.

### Address normalization

Для rank-2 strided memref вычисляется:

```text
element_offset = offset + index0 * stride0 + index1 * stride1
byte_offset = element_offset * element_size_bytes
```

В trace не сохраняется JIT pointer. Semantic address задаётся парой
`(memref_id, byte_offset)`.

### Access identity

Текущий ключ:

```text
(root_memref_id, access_kind, ordinal)
```

Ключ устойчив для текущих tiling/interchange примеров, где transformations не
клонируют сами logical access sites. Он недостаточен для fusion, unrolling и
других transformations, которые меняют число операций.

### Provenance marker

Каждый semantic memory op получает fused location metadata:

```text
cachescope.access_id
cachescope.memref_id
```

Upstream lowering переносит location на созданный LLVM load/store. Если две
стадии выполняются разными процессами `mlir-opt`, промежуточный файл должен
записываться с `--mlir-print-debuginfo`.

## 5. LLVM coverage/fallback pass

Pass запускается после полного conversion в LLVM dialect:

```text
cachescope-instrument-llvm-fallback{target-func=kernel}
```

Алгоритм:

1. найти все явные memory operations внутри LLVM target function;
2. пропустить load/store с semantic marker;
3. вставить fallback call для немаркированных load/store/atomic operations;
4. отклонить операцию с неизвестной memory policy;
5. отклонить неизвестный direct или indirect call.

Разрешённые calls ограничены allocation lifecycle и CacheScope runtime:
`malloc`, `calloc`, `aligned_alloc`, `free` и trace ABI.

Fallback ID начинается с `2^32`, чтобы не пересекаться с обычным пространством
semantic access IDs в текущем MVP.

## 6. Runtime ABI

Semantic ABI:

```c
void __cachescope_trace_v1(
    int64_t access_id,
    int64_t memref_id,
    int64_t byte_offset,
    int64_t size_bytes,
    int64_t access_kind,
    int64_t index0,
    int64_t index1);
```

Fallback ABI:

```c
void __cachescope_trace_fallback_v1(
    int64_t access_id,
    int64_t process_address,
    int64_t size_bytes,
    int64_t access_kind);
```

Lifecycle ABI:

```c
void __cachescope_trace_reset_v1(void);
void __cachescope_trace_flush_v1(void);
```

`atexit` остаётся страховкой для standalone/JIT процесса. Python вызывает
reset/flush явно, потому что интерпретатор продолжает жить после kernel call.

## 7. Event ordering и buffering

Runtime буферизует события в памяти. File I/O на каждом access изменил бы
исполнение значительно сильнее и не нужен для offline replay.

- `sequence` назначается в порядке runtime calls;
- `total_events` считает все события;
- `recorded_events` ограничивается `CACHESCOPE_TRACE_LIMIT`;
- `truncated` явно сообщает о потере хвоста;
- runtime сейчас не thread-safe.

## 8. PyTorch frontend

Текущий frontend намеренно AOT и сохраняет каждый IR stage:

```text
torch.nn.Module.forward
  → Linalg-on-Tensors
  → baseline/tile4/tile4-ikj
  → one-shot bufferization
  → C-compatible ranked memref wrapper
  → loops
  → semantic instrumentation
  → LLVM coverage pass
  → LLVM IR/object/shared library
  → ctypes call
```

Wrapper принимает два input и один output buffer. Возвращённый MLIR buffer
копируется в Python output и освобождается. Wrapper не входит в `target-func`,
поэтому интеграционные load/store не попадают в kernel trace.

PyTorch одновременно является понятным frontend и numerical oracle:
`torch.testing.assert_close` проверяет каждый compiled variant.

## 9. Отказ вместо неполной трассы

Основной инвариант проекта:

> Если CacheScope не знает семантику memory operation, compilation должна
> завершиться ошибкой, а не создать правдоподобную неполную трассу.

Поэтому текущая ограниченная поддержка безопаснее широкого best-effort режима.
Новая операция добавляется только вместе с policy, ABI mapping и тестом.

## 10. Расширение

Для добавления новой memory operation нужно определить:

1. является ли событие semantic или fallback;
2. buffer identity либо pointer address;
3. точный access size;
4. read/write/read_write kind;
5. поведение mask, vector lanes и split accesses;
6. стабильный access ID;
7. positive, negative и execution tests.

Практические workflows описаны в [INTEGRATION.md](INTEGRATION.md).
