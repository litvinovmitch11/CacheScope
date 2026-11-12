# CacheScope CLI

Эта страница собирает поддерживаемые командные точки входа. Основной
пользовательский сценарий запускается через Docker Project Studio; CLI нужен
для автоматизации, интеграции и диагностики отдельных стадий.

Все команды ниже выполняются из корня репозитория.

## 1. Сборка toolchain

```bash
scripts/install-llvm-dependencies.sh
cmake -S . -B build -G Ninja \
  -DMLIR_DIR=/usr/lib/llvm-22/lib/cmake/mlir \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build --parallel 3
```

Основные артефакты:

```text
build/lib/CacheScopePassPlugin.so
build/compiler/runtime/libcachescope_trace_runtime.so
build/bin/cachescope-program
build/runner/cachescope-runner
build/runner/cachescope_runner_server
```

На macOS shared libraries имеют расширение `.dylib`. Production Docker image
использует Linux paths `build/app/...`.

## 2. Compiler, instrumentation и trace

Поддерживаемый reference driver строит три MLIR-варианта одного matmul,
инструментирует, исполняет и валидирует их:

```bash
python3 examples/trace_demo.py
```

Результат появляется в `output/trace-demo/`: для каждого варианта сохраняются
`input.mlir`, снимки стадий, executable MLIR и `trace.json`.

С уже собранными plugin/runtime:

```bash
python3 examples/trace_demo.py \
  --plugin build/lib/CacheScopePassPlugin.so \
  --runtime build/compiler/runtime/libcachescope_trace_runtime.so
```

Driver является исполняемой спецификацией полного порядка:

```text
origin/source tags
  → Transform Dialect
  → linalg-to-loops
  → cachescope-instrument-memory
  → LLVM lowering
  → cachescope-instrument-llvm-fallback
  → mlir-runner + trace runtime
```

### Прямой вызов pass plugin

Semantic MemRef instrumentation одной функции:

```bash
/usr/lib/llvm-22/bin/mlir-opt input.mlir \
  --load-pass-plugin=build/lib/CacheScopePassPlugin.so \
  --pass-pipeline='builtin.module(cachescope-instrument-memory{target-func=kernel})' \
  --mlir-print-debuginfo \
  -o instrumented.mlir
```

LLVM fallback после conversion:

```bash
/usr/lib/llvm-22/bin/mlir-opt input-llvm.mlir \
  --load-pass-plugin=build/lib/CacheScopePassPlugin.so \
  --pass-pipeline='builtin.module(cachescope-instrument-llvm-fallback{target-func=kernel})' \
  --mlir-print-debuginfo \
  -o instrumented-llvm.mlir
```

Эти команды показывают интерфейс plugin, но сами по себе не создают полный
trace: для этого нужны source tags, lowering, runtime linkage и native
execution. Используйте `examples/trace_demo.py` как эталон orchestration.
Порядок passes и fail-closed правила описаны в
[compiler architecture](../compiler/cs-trace/ARCHITECTURE.md).

### Проверка и C-wrapper готовой MLIR-программы

Advanced MLIR/NPZ pipeline использует небольшой helper:

```bash
build/bin/cachescope-program inspect input.mlir
build/bin/cachescope-program wrap input.mlir kernel wrapped.mlir
```

`inspect` печатает JSON со списком функций, аргументов и причин
неподдерживаемости. `wrap` оставляет выбранную функцию и добавляет C-compatible
entry point. В этом режиме поддерживаются функции без return values со
статическими identity-layout `memref<...xf32>` аргументами; результаты нужно
передавать как output memref.

## 3. Проверка trace

```bash
python3 scripts/validate_trace.py output/trace-demo/baseline/trace.json
```

Validator не требует LLVM. Он проверяет schema, status, static access table,
события и source map. Runner дополнительно отклоняет invalid, incomplete и
truncated traces.

## 4. Batch cache simulation

Один L1:

```bash
build/runner/cachescope-runner \
  --trace output/trace-demo/baseline/trace.json \
  --output-dir output/trace-demo/simulation/baseline-l1 \
  --cache-name L1 \
  --size-bytes 32768 \
  --line-size-bytes 64 \
  --associativity 8 \
  --replacement-policy lru
```

Многоуровневая иерархия:

```bash
build/runner/cachescope-runner \
  --trace output/trace-demo/baseline/trace.json \
  --output-dir output/trace-demo/simulation/baseline-l1-l2-l3 \
  --cache-level L1:32768:64:8:lru \
  --cache-level L2:262144:64:8:lru \
  --cache-level L3:8388608:64:16:random:42
```

Формат уровня: `NAME:SIZE:LINE:WAYS:POLICY[:SEED]`. Доступны `lru`,
`fifo`, `random` и `plru4`; PLRU4 требует четыре way. Все уровни должны
иметь одинаковый line size.

Полные write/allocation и inclusion policies передаются через JSON:

```bash
build/runner/cachescope-runner \
  --trace output/trace-demo/baseline/trace.json \
  --output-dir output/trace-demo/simulation/custom \
  --config path/to/cache-config.json
```

`--config` принимает объект с `levels` и необязательным `hierarchy` из
[runner HTTP contract](RUNNER_API.md). Каждая команда создаёт
`statistics.json` и `events.json`. Сравнивайте schedules только при
одинаковых inputs, buffer placement и полной cache configuration.

Полный справочник: [runner CLI](../runner/README.md#cli) и
[cache models](CACHE_MODELS.md).

## 5. Полный reference E2E

```bash
python3 examples/product-e2e/run.py
```

Driver вызывает compiler flow, cache runner и generator автономного HTML. Его
результат генерируется локально в `output/product-e2e/` и не хранится в Git.
Это regression/reference flow над небольшим matmul, не основной Torch Studio.

Пользовательский Torch tile sweep можно запустить без UI:

```bash
python3 -m venv venv
venv/bin/pip install -r requirements-torch-demo.lock
venv/bin/python examples/ml-locality/depthwise-temporal-blocking/run.py
```

Он сравнивает baseline и тайлы `6/10/14` на одинаковых inputs и cache
profiles. Параметры перечислены в
[описании эксперимента](../examples/ml-locality/depthwise-temporal-blocking/README.md).

## 6. Локальный HTTP backend и Stepper API

```bash
build/runner/cachescope_runner_server --app-dir .
```

По умолчанию server слушает <http://127.0.0.1:9055>. Без `--app-dir`
доступны готовые trace sessions, но не Project Studio/MLIR experiments.

Пример создания сессии:

```bash
curl -s http://127.0.0.1:9055/api/v1/sessions \
  -H 'Content-Type: application/json' \
  -d '{"trace_path":"/absolute/path/trace.json","levels":[{"name":"L1","size_bytes":32768,"line_size_bytes":64,"associativity":8,"replacement_policy":"LRU"}]}'
```

Дождитесь `ready` через `GET /api/v1/sessions/<id>`, затем вызывайте
`/step`, `/state`, `/statistics` и `/instructions`. Revision, лимиты,
reset/delete и ошибки описаны в [RUNNER_API.md](RUNNER_API.md).

Project/MLIR job API описан отдельно в [EXPERIMENT_API.md](EXPERIMENT_API.md).

## 7. Автономный HTML-отчёт

Для уже подготовленной experiment directory:

```bash
npm --prefix ui ci
npm --prefix ui run report -- \
  --input ../output/experiment \
  --output ../output/experiment-report.html
```

Interactive Project Studio использует server-side artifacts и отдельный report
screen. Standalone HTML предназначен для переноса результатов без backend и не
содержит Stepper.

## 8. Независимая валидация

Cachegrind:

```bash
python3 scripts/compare_cachegrind.py \
  --output /tmp/cachescope-cachegrind-results
```

DynamoRIO drcachesim:

```bash
python3 scripts/compare_drcachesim.py \
  --dynamorio /path/to/DynamoRIO \
  --output /tmp/cachescope-drcachesim-results
```

Semantic trace против drmemtrace:

```bash
python3 tests/compare_drmemtrace.py \
  --mlir-bin /usr/lib/llvm-22/bin \
  --plugin build/lib/CacheScopePassPlugin.so \
  --runtime build/compiler/runtime/libcachescope_trace_runtime.so \
  --dynamorio /path/to/DynamoRIO \
  --output-dir /tmp/cachescope-drmemtrace-comparison
```

Эти suites требуют внешние Valgrind/DynamoRIO installations и проверяют только
условия, перечисленные в документах
[Cachegrind](CACHEGRIND_COMPARISON.md),
[drcachesim](DRCACHESIM_COMPARISON.md) и
[drmemtrace](DRMEMTRACE_COMPARISON.md). Они не являются hardware benchmark.

## 9. Проверки репозитория

```bash
scripts/check.sh
scripts/check.sh --with-lint
scripts/check.sh --with-torch
```

`--with-torch` требует подготовленный `venv` с
`requirements-torch-demo.lock`. Полная матрица проверок описана в
[development guide](DEVELOPMENT.md).
