# Compiler instrumentation

Подробная системная архитектура находится в
[`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md). Этот файл фиксирует
локальные обязанности plugin.

## Passes

### `cachescope-instrument-memory`

- input: SCF/MemRef IR после lowering structured operations;
- scope: одна `func.func`, выбранная через `target-func`;
- output: semantic runtime calls и location provenance markers;
- supported data operations: rank-2 `memref.load/store`.

### `cachescope-instrument-llvm-fallback`

- input: LLVM dialect после conversion;
- scope: одна `llvm.func`, выбранная через `target-func`;
- marked load/store: считаются уже покрытыми;
- unmarked load/store/atomics: получают pointer fallback calls;
- unknown direct memory effect или call: pass failure.

## Required order

```text
structured lowering
  → cachescope-instrument-memory
  → canonicalize/cse
  → LLVM conversion
  → cachescope-instrument-llvm-fallback
```

При записи промежуточного semantic IR используется
`--mlir-print-debuginfo`, иначе location marker не попадёт в следующий процесс.

## Invariants

- instrumentируется только target function;
- pass нельзя безопасно запускать повторно;
- unsupported access не пропускается молча;
- semantic и fallback ID spaces не пересекаются в текущем MVP;
- plugin не выполняет I/O и не хранит runtime state.

Текущие ограничения перечислены в
[`docs/SUPPORT_MATRIX.md`](../../docs/SUPPORT_MATRIX.md).
