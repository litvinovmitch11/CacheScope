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

Полная compiler-проверка ниже относится к compiler/runtime; для изменений
раннера обязательны его четыре CTest suites и `git diff --check`.

## Сборка с чистой машины

Нужны Git, CMake 3.20+, Ninja, Python 3.10+ и C/C++ compiler. На Ubuntu/Debian
достаточно базовых пакетов:

```bash
sudo apt-get install git cmake ninja-build build-essential python3-venv
```

Клонируйте репозиторий вместе с закреплённым LLVM:

```bash
git clone --recurse-submodules https://github.com/litvinovmitch11/CacheScope.git
cd CacheScope
```

Если репозиторий уже был клонирован без submodules:

```bash
git submodule update --init --recursive
```

Соберите ровно те LLVM/MLIR/Clang tools, которые нужны CacheScope:

```bash
scripts/build-llvm.sh
```

Это самая долгая стадия. Она создаёт `llvm-project/build` и собирает `mlir-opt`,
`mlir-transform-opt`, `mlir-translate`, `mlir-runner`, `llc`, `FileCheck`,
`clang-format` и `clang-tidy`. Нужны несколько гигабайт свободного места;
длительность зависит от числа CPU cores.

LLVM закреплён в Git как submodule на версии `24.0.0git`, commit
`40e5c847efaca209a9823e950fc5f26dc60cad4e`. Ожидаемые значения хранятся в
`cmake/LLVMVersion.cmake`; root configure и `scripts/build-llvm.sh` прекращают
работу при несовпадении версии или commit. LLVM собирается отдельным CMake
project.
CacheScope подключается к готовому `MLIRConfig.cmake` через `find_package`.
Так root configure остаётся быстрым, а изменение plugin не пересобирает весь
LLVM. Встраивание `llvm-project/llvm` через `add_subdirectory` здесь намеренно
не используется.

После этого соберите и проверьте CacheScope:

```bash
cmake -S . -B build -G Ninja \
  -DMLIR_DIR="$PWD/llvm-project/build/lib/cmake/mlir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Эквивалентная короткая команда:

```bash
scripts/check.sh
```

## PyTorch frontend

Compiler и MLIR тесты не требуют PyTorch. Для полного frontend demo создайте
окружение отдельно:

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
- `llvm-project/build/` — сборка закреплённого LLVM submodule;
- `output/` — generated MLIR, LLVM IR, traces, input tensors и результаты
  экспериментов.

Все три директории локальные и не коммитятся. Удаление `output/` не требует
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

Обе цели используют инструменты из `llvm-project/build/bin`, `.clang-format` в
LLVM style и `.clang-tidy` в корне проекта. Полная проверка перед commit:

```bash
scripts/check.sh --with-lint
git diff --check
```

## Тесты

- `cachescope.pass-suite` — positive/negative pass behavior и marker lowering;
- `cachescope.runtime-suite` — ABI, JSON, truncation, reset/flush;
- `cachescope.end-to-end` — baseline/tiled compilation, execution и trace diff;
- `cachescope.documentation-suite` — существование локальных Markdown links.
- `cachescope.runner-*` — integration, validation, random-miss и streaming;
  запускаются из отдельного `build/runner`.

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
6. При изменении порядка событий объяснить ожидаемую разницу в тесте.

## Установка

```bash
cmake --install build --prefix /desired/prefix
```

Устанавливаются plugin, trace-producer runtime и публичный ABI header:

```text
lib/cachescope/CacheScopePassPlugin.so
lib/libcachescope_trace_runtime.so
include/cachescope/TraceABI.h
```
