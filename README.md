# CacheScope

CacheScope показывает, как compiler transformations меняют порядок обращений
к памяти. Текущая вертикаль компилирует MLIR kernel, выполняет его после
гибридной MemRef/LLVM-инструментации и создаёт воспроизводимый
`cachescope.trace.v1` для будущего cache simulator и UI.

## Что уже работает

- out-of-tree MLIR pass plugin;
- semantic trace для `memref.load/store` с `memref_id` и byte offset;
- LLVM fallback для `load/store/atomicrmw/cmpxchg`;
- fail-closed диагностика неизвестных memory effects;
- static access/source table и producer metadata;
- trace status: `valid`, `complete`, `truncated`, `errors`;
- JSON Schema, standalone validator и compatibility fixtures;
- stage-aware mapping от input `linalg` operation до конкретных
  `memref.load/store` и `llvm.load/store`;
- один канонический baseline/tile/interchange example;
- pass, runtime, schema, end-to-end и documentation tests;
- SetAssociativeCacheLevel с LRU, exclusive CacheHierarchy и независимый trace
  runner со статистикой по уровням, инструкциям и подробным временным рядом для UI.

Точные границы перечислены в [support matrix](docs/SUPPORT_MATRIX.md).

## Раннер и кеш без LLVM

```bash
cmake -S runner -B build/runner -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/runner
ctest --test-dir build/runner --output-on-failure
```

API и smoke-тест: [runner](runner/README.md). Владение уровнями и exclusive:
[CacheHierarchy](simulator/README.md). [Контракт UI](docs/SIMULATION_FORMAT.md)
и [дополнительные поля отчёта](docs/STATISTICS_FORMAT.md).

## Быстрый старт

Клонируйте репозиторий и установите закреплённые LLVM/MLIR 22.1.8 packages
для Ubuntu 24.04:

```bash
git clone https://github.com/litvinovmitch11/CacheScope.git
cd CacheScope
scripts/install-llvm-dependencies.sh
```

Запустите канонический пример:

```bash
python3 examples/trace_demo.py
```

Одна команда:

1. собирает plugin и trace runtime;
2. создаёт `baseline`, `tile4` и `tile4-ikj`;
3. инструментирует каждый вариант через plugin;
4. выполняет код и проверяет численный результат;
5. создаёт и валидирует три traces;
6. проверяет, что compiler schedules действительно изменили dynamic order.

Результат:

```text
output/trace-demo/
  summary.json
  baseline/{input.mlir,01-transformed.mlir,...,trace.json}
  tile4/{input.mlir,01-transformed.mlir,...,trace.json}
  tile4-ikj/{input.mlir,01-transformed.mlir,...,trace.json}
```

Подробное описание стадий и артефактов находится в
[examples/README.md](examples/README.md).

## Сборка и проверки

```bash
cmake -S . -B build -G Ninja \
  -DMLIR_DIR=/usr/lib/llvm-22/lib/cmake/mlir \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Эквивалентная команда:

```bash
scripts/check.sh
```

Перед merge:

```bash
scripts/check.sh --with-lint
git diff --check
```

Готовый trace можно проверить без LLVM libraries:

```bash
python3 scripts/validate_trace.py output/trace-demo/baseline/trace.json
```

## Компоненты

```text
compiler/cs-trace/     MLIR pass plugin
compiler/runtime/      runtime trace producer
simulator/             include/lib/tests: уровни кеша, политики и иерархия
runner/                include/lib/tests: replay, статистика и JSON-отчёты
schemas/               общие контракты и fixtures
examples/              один канонический pipeline и optional PyTorch frontend
tests/                 compiler, ABI и проверки общих контрактов
docs/                  архитектура и межкомпонентные контракты
output/                локальные generated artifacts, ignored
```

## Дополнительный PyTorch frontend

Основной пример не требует Python ML packages. Отдельная PyTorch-интеграция
проверяется так:

```bash
python3 -m venv venv
venv/bin/pip install -r requirements-torch-demo.lock
scripts/check.sh --with-torch
```

Она экспортирует `torch.nn.Module` через torch-mlir и использует тот же plugin
и trace contract. Это расширенная интеграция, а не основной путь знакомства с
репозиторием.

## Документация

- [канонический пример](examples/README.md);
- [архитектура](docs/ARCHITECTURE.md);
- [сборка и разработка](docs/DEVELOPMENT.md);
- [формат trace](docs/TRACE_FORMAT.md);
- [stage-aware source mapping](docs/SOURCE_MAPPING.md);
- [командная интеграция](docs/INTEGRATION.md);
- [support matrix](docs/SUPPORT_MATRIX.md);
- [roadmap](ROADMAP.md).

CacheScope сейчас рассчитан на однопоточные CPU kernels. Это не
cycle-accurate simulator и не предсказатель wall-clock производительности.
