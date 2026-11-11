# Compiler instrumentation

Подробная системная архитектура находится в
[`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md). Этот файл фиксирует
локальные обязанности plugin.

## Passes

### `cachescope-assign-origins`

- input: high-level target function после input location snapshot;
- назначает deterministic `origin_id` операциям с direct memory effects;
- сохраняет исходное полное имя MLIR operation.

### `cachescope-tag-stage`

- запускается после `snapshot-op-locations` на каждой UI-visible стадии;
- связывает stage ID, operation name и сохранённую line/column;
- tagged locations переносятся стандартными MLIR transformations.

### `cachescope-instrument-memory`

- input: SCF/MemRef IR после lowering structured operations;
- scope: одна `func.func`, выбранная через `target-func`;
- output: semantic runtime calls и location provenance markers;
- supported data operations: arbitrary-rank strided `memref.load/store`.

### `cachescope-instrument-llvm-fallback`

- input: LLVM dialect после conversion;
- scope: одна `llvm.func`, выбранная через `target-func`;
- marked load/store: считаются уже покрытыми;
- unmarked load/store/atomics: получают pointer fallback calls;
- все semantic/fallback sites регистрируются в прологе вместе с location;
- source-map metadata передаётся в runtime типизированными ABI calls, без
  готовых JSON-фрагментов;
- unknown direct memory effect или call: pass failure.

## Required order

```text
structured lowering
  → cachescope-instrument-memory
  → canonicalize/cse
  → LLVM conversion
  → cachescope-instrument-llvm-fallback
```

Полный stage-aware порядок канонического driver-а:

```text
input snapshot → assign origins → tag input
  → transform snapshot/tag
  → linalg-to-loops snapshot/tag
  → semantic instrumentation snapshot/tag
  → LLVM lowering snapshot/tag
  → LLVM fallback + static source-map registration
```

При записи промежуточного semantic IR используется
`--mlir-print-debuginfo`, иначе location marker не попадёт в следующий процесс.

## Invariants

- instrumentируется только target function;
- pass нельзя безопасно запускать повторно;
- unsupported access не пропускается молча;
- semantic и fallback ID spaces не пересекаются в текущем MVP;
- static access table не зависит от выполненных control-flow paths;
- JSON сериализует только runtime после проверки ссылок и полноты mapping;
- при включённом `payload-fingerprint` потеря provenance завершает pass
  ошибкой;
- plugin не выполняет I/O и не хранит runtime state.

Текущие ограничения перечислены в
[`docs/SUPPORT_MATRIX.md`](../../docs/SUPPORT_MATRIX.md).
