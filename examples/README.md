# Канонический пример trace producer

Основная пользовательская точка входа — один Python driver:

```bash
python3 examples/trace_demo.py
```

Он самостоятельно собирает CacheScope plugin/runtime, компилирует один matmul
тремя способами, запускает инструментированный код, проверяет численный
результат и валидирует все traces.

## Что именно запускается

Для всех вариантов используется один payload
[`workloads/matmul.mlir`](workloads/matmul.mlir): матрицы `16×16 f32`
инициализируются единицами, после чего `C[0, 0]` должен быть равен `16`.

Варианты отличаются только compiler schedule:

| Variant | Transform | Ожидаемый порядок |
|---|---|---|
| `baseline` | без Transform Dialect schedule | обычный `i → j → k` |
| `tile4` | tiles `4×4×4` | tiled `i → j → k` |
| `tile4-ikj` | tiles `4×4×4`, interchange | tiled `i → k → j` |

Каждый вариант проходит одинаковые стадии:

```text
payload / Transform Dialect
  → origin assignment + tagged stage snapshots
  → linalg-to-loops
  → cachescope-instrument-memory
  → LLVM lowering
  → cachescope-instrument-llvm-fallback
  → mlir-runner + trace runtime
  → scripts/validate_trace.py contract checks
```

Driver требует, чтобы результат каждого запуска был `16`, trace имел
`valid = true` и `complete = true`, а все события были semantic
`memref_offset`. После этого он сравнивает три динамических порядка и требует,
чтобы каждая пара действительно различалась.

## Артефакты

По умолчанию результат записывается в `output/trace-demo/`:

```text
summary.json
baseline/
  input.mlir
  01-transformed.mlir
  02-loops.mlir
  03-instrumented.mlir
  04-llvm.mlir
  trace.json
tile4/
  ...
tile4-ikj/
  ...
```

`trace.json` вместе с пятью MLIR dumps образует переносимый UI bundle.
`source_map.stages[].path` ссылается на эти файлы, а line/column — на их точные
байты. Скрытые `.NN-*-tagged.mlir` являются внутренними файлами driver и не
входят в контракт. `summary.json` содержит пути к traces, их размеры/status и
позицию первой динамической разницы для каждой пары variants.

Для всех трёх variants `origin_id = 0` указывает на один input
`linalg.matmul`. После выбора `after-linalg-to-loops` UI получает четыре
раздельных sites: три `memref.load` и один `memref.store`.

Trace можно повторно проверить отдельно:

```bash
python3 scripts/validate_trace.py output/trace-demo/baseline/trace.json
```

## Настройка путей

Для стандартной структуры репозитория аргументы не нужны. При внешней сборке:

```bash
python3 examples/trace_demo.py \
  --llvm-build-dir /path/to/llvm-build \
  --build-dir /path/to/cachescope-build \
  --output-dir /tmp/cachescope-traces
```

В CI driver может использовать уже собранные artifacts:

```bash
python3 examples/trace_demo.py \
  --llvm-build-dir "$PWD/llvm-project/build" \
  --plugin "$PWD/build/lib/CacheScopePassPlugin.so" \
  --runtime "$PWD/build/compiler/runtime/libcachescope_trace_runtime.so"
```

`--plugin` и `--runtime` всегда передаются вместе.

## Продвинутый PyTorch frontend

[`torch-demo/run.py`](torch-demo/run.py) сохраняется как дополнительная
интеграционная проверка PyTorch → torch-mlir → native execution. Он не нужен,
чтобы понять или проверить основной trace pipeline, и требует отдельного
окружения из `requirements-torch-demo.lock`.

```bash
scripts/check.sh --with-torch
```

Негативные pass cases, LLVM fallback и runtime ABI являются тестовыми
сценариями и находятся в `compiler/cs-trace/test` и `tests`, а не среди
пользовательских примеров.
