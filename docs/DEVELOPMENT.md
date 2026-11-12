# Сборка и разработка

## Только trace runner

Для работы со статистикой готовых трасс LLVM/MLIR не нужен:

```bash
cmake -S runner -B build/runner -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/runner
ctest --test-dir build/runner --output-on-failure
```

Нужны C++17 compiler, CMake 3.20+ и Ninja. nlohmann/json скачивается при
configure; автономная сборка и подключение к приложению описаны в
[runner README](../runner/README.md).

Уровни кеша, replacement policies и иерархию можно собрать отдельно, без JSON:

```bash
cmake -S simulator -B build/simulator -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/simulator
ctest --test-dir build/simulator --output-on-failure
```

Публичные заголовки каждого модуля находятся в его `include/`, реализации —
в `lib/`, собственные тесты — в `tests/`. Общие контрактные fixtures находятся
в `schemas/fixtures/`.

Полная compiler-проверка ниже относится к compiler/runtime; для изменений
раннера обязательны CTest suites раннера, иерархии и симулятора и `git diff --check`.

## Сборка с чистой машины

Нужны Git, CMake 3.20+, Ninja, Python 3.10+ и C/C++ compiler. Поддерживаемое
reference-окружение compiler pipeline — Ubuntu 24.04 (Noble), amd64.

Клонируйте репозиторий:

```bash
git clone https://github.com/litvinovmitch11/CacheScope.git
cd CacheScope
```

Установите готовые LLVM/MLIR/Clang 22.1.8 packages:

```bash
scripts/install-llvm-dependencies.sh
```

Скрипт подключает официальный apt.llvm.org repository, проверяет fingerprint
его signing key и устанавливает точные Debian package versions из
`cmake/LLVMVersion.cmake`. LLVM source и build tree не скачиваются. CacheScope
подключается к установленному `MLIRConfig.cmake` через `find_package`.

LLVM закреплён на release `22.1.8`, commit
`ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`. Эти значения одновременно задают
CMake compatibility check и producer metadata в trace. При обновлении LLVM
нужно согласованно изменить major, package version, commit и Debian version в
`cmake/LLVMVersion.cmake`, после чего полностью прогнать compiler suite.

На другой ОС установите LLVM/MLIR 22.1.8 штатным package manager и задайте
`LLVM_ROOT` на install prefix. По умолчанию используется `/usr/lib/llvm-22`.

После этого соберите и проверьте CacheScope:

```bash
cmake -S . -B build -G Ninja \
  -DMLIR_DIR=/usr/lib/llvm-22/lib/cmake/mlir \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Эквивалентная короткая команда:

```bash
scripts/check.sh
```

Для compiler/runtime CI и локальной проверки без повторной сборки независимых
runner/simulator targets:

```bash
CACHESCOPE_BUILD_CACHE_MODEL=OFF scripts/check.sh --with-lint
```

Этот режим по-прежнему запускает pass, runtime, schema, documentation и полный
baseline/tile/interchange end-to-end. Runner, hierarchy и simulator проверяются
отдельным быстрым workflow без LLVM. GitHub Actions workflow
`.github/workflows/compiler-tests.yml` использует этот режим и собирает
CacheScope против готовых закреплённых apt.llvm.org packages. CI не скачивает
LLVM source, не собирает LLVM и не кеширует LLVM build tree.

Минимальный compiler reference flow использует ту же сборку:

```bash
python3 examples/trace_demo.py
```

Он создаёт три variants, traces и `output/trace-demo/summary.json`. Подробное
описание находится в [`examples/README.md`](../examples/README.md).

Полный CLI regression flow дополнительно собирает runner, воспроизводит три
trace через демонстрационный L1 и создаёт standalone UI report:

```bash
python3 examples/product-e2e/run.py
```

Результат и все промежуточные JSON находятся в `output/product-e2e/`.
Параметры и требования Node.js описаны в
[`examples/product-e2e/README.md`](../examples/product-e2e/README.md).
Примеры LRU/FIFO/Random/PLRU4, разных геометрий и hardware-like конфигурации находятся
в [`CACHE_MODELS.md`](CACHE_MODELS.md).

Основной пользовательский сценарий — локальный Docker Project Studio. Его
запуск и контракт проекта находятся в [корневом README](../README.md).

## PyTorch frontend

Production Docker image уже содержит pinned PyTorch environment. Для нативной
developer-проверки frontend и worker создайте окружение отдельно:

```bash
python3 -m venv venv
venv/bin/pip install -r requirements-torch-demo.lock
scripts/check.sh --with-torch
```

`torch-mlir` жёстко связан с версиями PyTorch/LLVM. При обновлении зависимостей
обязательно запускайте полный Torch pipeline.

## Build и output

Директории имеют разные назначения:

- `build/` — только CMake/Ninja artifacts, libraries и `compile_commands.json`;
- `output/` — generated MLIR, LLVM IR, traces, input tensors и результаты
  экспериментов.

Обе директории локальные и не коммитятся. Удаление `output/` не требует
пересборки CacheScope; удаление `build/` требует повторной CMake-сборки.
Путь экспериментов можно заменить через
`CACHESCOPE_OUTPUT_DIR=/path/to/output`.

## compile_commands, format и tidy

Root CMake всегда включает `CMAKE_EXPORT_COMPILE_COMMANDS`. После configure база
команд находится в:

```text
build/compile_commands.json
```

Для IDE/clangd укажите `build` как compilation database directory. Проверки:

```bash
cmake --build build --target format-check
cmake --build build --target tidy
```

Автоматическое форматирование:

```bash
cmake --build build --target format
```

Обе цели используют инструменты из `/usr/lib/llvm-22/bin`, `.clang-format` в
LLVM style и `.clang-tidy` в корне проекта. Полная проверка перед commit:

```bash
scripts/check.sh --with-lint
git diff --check
```

## Тесты

- `cachescope.pass-suite` — positive/negative pass behavior и marker lowering;
- `cachescope.runtime-suite` — ABI, JSON, truncation, reset/flush;
- `cachescope.end-to-end` — baseline/tiled compilation, execution и trace diff;
- `cachescope.schema-suite` — schema fixtures, compatibility и invariants;
- `cachescope.documentation-suite` — существование локальных Markdown links.
- `cachescope.runner-*` — integration, real-cache, fifo-cache, random-cache,
  validation, random-miss, streaming и CLI smoke для LRU/FIFO/Random/PLRU4
  и L1/L2/L3;
- `cachescope.hierarchy` — exclusive-переносы, dirty и реальный LRU;
- `SetAssociativeCacheLevelTest.*`, `LRUTest.*` — upstream-тесты симулятора.

Сборка `build/runner` включает все три компонента без LLVM. CI выполняет те же
тесты в GCC Release и Clang ASan/UBSan.

Запуск одного теста:

```bash
ctest --test-dir build -R pass-suite --output-on-failure
```

## Добавление compiler behavior

1. Изменить implementation в `compiler/cs-trace/lib`.
2. Добавить минимальный `.mlir` fixture в `compiler/cs-trace/test`.
3. Для ошибки проверить точную диагностику.
4. Подключить fixture в `tests/pass_suite.py`.
5. При изменении ABI обновить `compiler/runtime`, runtime suite и trace format.
6. При изменении trace contract обновить JSON Schema, validator и fixtures.
7. При изменении порядка событий объяснить ожидаемую разницу в тесте.

## Установка

```bash
cmake --install build --prefix /desired/prefix
```

Устанавливаются plugin, trace-producer runtime, schema и compiler ABI:

```text
lib/cachescope/CacheScopePassPlugin.so
lib/libcachescope_trace_runtime.so
bin/cachescope-runner
include/cachescope/TraceABI.h
share/cachescope/schema/cachescope.trace.v1.schema.json
```
