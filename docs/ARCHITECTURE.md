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

Cache simulator и UI являются следующими независимыми потребителями trace.
В `simulator/` определены интерфейсы и реализации set-associative cache level и
LRU policy; целевое разделение иерархии и раннера описано в разделе 13.

## 2. Компоненты

### `compiler/cs-trace`

Out-of-tree plugin для `mlir-opt`. Регистрирует четыре module passes:

- `cachescope-assign-origins`;
- `cachescope-tag-stage`;
- `cachescope-instrument-memory`;
- `cachescope-instrument-llvm-fallback`.

Plugin не занимается JSON и файловым вводом-выводом. Его задача — определить
origins и access sites, вычислить типизированные аргументы ABI и вставить
runtime calls.

### `compiler/runtime`

Небольшая shared library без зависимости от MLIR. Она принимает типизированные
static metadata и события через C ABI, проверяет целостность source mapping,
назначает динамический `sequence`, применяет trace limit и сериализует JSON.
Runtime расположен внутри `compiler/`, потому что является последней частью
trace producer. Он не является runtime cache emulator.

### `examples/trace_demo.py`

Канонический orchestration driver. Он строит baseline/tile/interchange одного
MLIR payload, запускает оба instrumentation passes, выполняет код, валидирует
traces и сравнивает dynamic order. PyTorch frontend оставлен дополнительной
интеграцией, а не основным способом понять систему.

### `examples/torch-demo`

Frontend и orchestration layer. Он экспортирует `torch.nn.Module` через
torch-mlir, запускает Transform Dialect, bufferization, instrumentation,
LLVM lowering, AOT compilation и вызов через `ctypes`.

### `tests`

Независимые уровни проверки:

- pass unit/integration tests;
- runtime ABI/serialization tests;
- schema/validator compatibility tests;
- полный canonical baseline/tile/interchange execution test;
- documentation link checks.

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
4. собрать static access descriptions и source locations;
5. вставить регистрации всех access sites в пролог target function;
6. отклонить операцию с неизвестной memory policy;
7. отклонить неизвестный direct или indirect call.

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
    int64_t access_kind);
```

Static access ABI:

```c
void __cachescope_register_access_v1(
    int64_t access_id,
    int64_t memref_id,
    int64_t access_kind,
    int64_t address_kind);
void __cachescope_register_source_v1(
    int64_t access_id,
    const char *source_location);
```

LLVM pass вставляет эти calls в пролог. Runtime поэтому знает sites, которые
не исполнились либо оказались за trace limit. Logical indices не входят в
dynamic ABI: cache contract использует нормализованный byte offset.

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
- `valid` отражает нарушения ABI, а `complete = valid && !truncated`;
- trace публикуется через временный файл;
- runtime сейчас не thread-safe.

## 8. Канонический compiler example

`examples/trace_demo.py` является executable specification основного пути:

```text
один matmul payload
  → baseline / tile4 / tile4-ikj
  → linalg-to-loops
  → semantic instrumentation
  → LLVM lowering и coverage audit
  → mlir-runner
  → trace validation и comparison
```

Тест `cachescope.end-to-end` вызывает этот же driver с уже собранными plugin и
runtime. Поэтому документационный пример и CI не расходятся на две копии
pipeline.

## 9. PyTorch frontend

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

## 10. Отказ вместо неполной трассы

Основной инвариант проекта:

> Если CacheScope не знает семантику memory operation, compilation должна
> завершиться ошибкой, а не создать правдоподобную неполную трассу.

Поэтому текущая ограниченная поддержка безопаснее широкого best-effort режима.
Новая операция добавляется только вместе с policy, ABI mapping и тестом.

## 11. Расширение

Для добавления новой memory operation нужно определить:

1. является ли событие semantic или fallback;
2. buffer identity либо pointer address;
3. точный access size;
4. read/write/read_write kind;
5. поведение mask, vector lanes и split accesses;
6. стабильный access ID;
7. positive, negative и execution tests.

Практические workflows описаны в [INTEGRATION.md](INTEGRATION.md).

## 12. Stage-aware source mapping

Optional `cachescope.source-map.v1` связывает high-level origins, operations в
сохранённых IR stages и static `access_id`. Dynamic events продолжают хранить
только `access_id`, поэтому mapping не зависит от event limit и truncation.

Контракт и UI navigation rules описаны в
[SOURCE_MAPPING.md](SOURCE_MAPPING.md). Producer использует MLIR location
snapshots, CacheScope origin/stage tags и fail-closed сборку mapping в финальном
LLVM pass. Канонический пример проверяет координаты во всех пяти dumps.

## 13. Симулятор кеша: разделение ответственности

Ниже описана целевая архитектура потребителя трассы. В текущей ветке доступны
`ICacheLevel`, `IReplacementPolicy`, `SetAssociativeCacheLevel` и `LRU` с
тестами. `CacheHierarchy` и cache runner ещё не реализованы; их сигнатуры здесь
обозначают целевые границы компонентов, а не уже доступный API.

```text
trace.v1 JSON
  → Cache runner: проверка трассы, адреса, статистика, JSON-отчёт
    → CacheHierarchy: read/write(address, size), политики между уровнями
      → ICacheLevel: lookup / touch / insert / invalidate / clear
        → IReplacementPolicy: on_hit / on_insert / on_invalidate / choose_victim / clear
```

### Ответственность и владение

| Компонент | За что отвечает | Чем владеет |
|---|---|---|
| Replacement policy | Выбор жертвы среди ways одного set, например LRU | Состояние порядка использования ways |
| Cache level | Размещение строк по sets/ways, valid/dirty, локальные операции над строкой | Строки и экземпляры replacement policy для каждого set |
| Cache hierarchy | Прохождение запроса через L1 → L2 → L3, заполнение, переносы, writeback и inclusive/exclusive-политики | Уровни кеша и их конфигурация: порядок, имена, размеры строк |
| Cache runner | Чтение и проверка трассы, назначение модельных адресов, вызов иерархии, статистика и отчёт | Состояние прогона, размещение буферов и агрегаты статистики |

Вызывающий код создаёт уровни и передаёт их во владение иерархии, например через
`unique_ptr<ICacheLevel>`. Раннер получает иерархию по ссылке без передачи
владения. Уровень знает только собственные строки и replacement policy;
политика замещения оперирует номерами ways и не знает адресов, dirty-флагов,
других уровней или формата трассы.

### Replacement policy

Контракт: [ReplacementPolicy.h](../simulator/include/cache_simulator/ReplacementPolicy.h).

- `on_hit(way)` обновляет порядок использования после обращения к существующей
  строке; его вызывает уровень при `touch`.
- `on_insert(way)` учитывает размещение новой строки и обновляет состояние
  переиспользуемого way.
- `on_invalidate(way)` удаляет way из replacement state; неизвестный way
  игнорируется. Уровень вызывает его при удалении существующей строки.
- `choose_victim()` выбирает way для вытеснения, когда свободных ways нет.
- `clear()` сбрасывает состояние политики данного set.

Свободный way выбирает сам уровень; valid/dirty хранятся в уровне. Состояние LRU
для разных sets должно быть независимым.

### Cache level

Контракт: [CacheLevel.h](../simulator/include/cache_simulator/CacheLevel.h).
Интерфейс одного уровня — `ICacheLevel`.

Все операции принимают начальный адрес строки, выровненный по размеру строки
данного уровня. Разбиение диапазона байтов выполняет иерархия.

- `lookup(address) const` только проверяет наличие. `LookupResult.set`
  определяется адресом даже при промахе; optional `way` присутствует при
  попадании. Метод `hit()` вычисляется как `way.has_value()`.
- `touch(address, type)` обновляет replacement state. При записи устанавливает
  `dirty = true`, при чтении сохраняет dirty. Отсутствующая строка приводит к
  `std::out_of_range` без изменения состояния.
- `insert(CacheLine)` размещает отсутствующую строку с переданным dirty-флагом.
  Сначала использует свободный way, иначе выбирает жертву. Возвращает set/way
  размещения и optional вытесненную строку с dirty. Вызывающий код гарантирует
  отсутствие вставляемой строки.
- `invalidate(address)` удаляет строку и возвращает её вместе с dirty; при
  отсутствии возвращает `nullopt`. Освобождённый way доступен для вставки.
- `clear()` удаляет строки и сбрасывает replacement state.

`lookup` не изменяет LRU: проверка наличия для управления иерархией сама по себе
не считается обращением программы. Для обслуженного попадания иерархия отдельно
вызывает `touch`. Между этими вызовами строка должна оставаться в кеше;
синхронизация параллельных обращений текущим контрактом не предусмотрена.

`CacheLine` хранит адрес и dirty-флаг, без значений данных. Dirty означает,
что строка содержит изменения, ещё не переданные нижележащему хранилищу.
Уровень возвращает вытесненную строку, а дальнейшую передачу организует иерархия.
