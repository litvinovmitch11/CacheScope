# CacheScope

CacheScope — trace producer для исследования того, как compiler
transformations меняют обращения к памяти. Текущая вертикаль принимает
PyTorch/MLIR kernel, выполняет его после гибридной MemRef/LLVM-инструментации и
создаёт воспроизводимый trace для будущего cache emulator.

Cache emulator и UI разрабатываются как отдельные компоненты и не зависят от
LLVM/MLIR libraries.

## Что работает

- PyTorch → torch-mlir → Linalg/SCF/MemRef → LLVM → native execution;
- semantic trace для `memref.load/store` с `memref_id` и byte offset;
- LLVM fallback для `load/store/atomicrmw/cmpxchg`;
- строгий отказ на неизвестных memory effects;
- baseline, tiling и loop interchange одного kernel;
- JSON trace, input manifest и точные входные tensors;
- pass, runtime, end-to-end и documentation tests.

Границы поддержки перечислены в [support matrix](docs/SUPPORT_MATRIX.md).

## С нуля до первого trace

Клонируйте LLVM submodule, закреплённый на `24.0.0git` commit
`40e5c847efaca209a9823e950fc5f26dc60cad4e`, и соберите необходимые
инструменты:

```bash
git clone --recurse-submodules https://github.com/litvinovmitch11/CacheScope.git
cd CacheScope
scripts/build-llvm.sh
```

Соберите CacheScope и запустите тесты:

```bash
cmake -S . -B build -G Ninja \
  -DMLIR_DIR="$PWD/llvm-project/build/lib/cmake/mlir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

То же самое одной командой после сборки LLVM:

```bash
scripts/check.sh
```

Подробная установка зависимостей и альтернативные команды находятся в
[development guide](docs/DEVELOPMENT.md).

## PyTorch demo

```bash
python3 -m venv venv
venv/bin/pip install -r requirements-torch-demo.lock
scripts/check.sh --with-torch
```

Результат находится в `output/torch-demo/`:

```text
input.json
inputs.npz
comparison.json
baseline/trace.json
tile4/trace.json
tile4-ikj/trace.json
```

Все варианты получают одинаковые входы и проверяются относительно PyTorch.

## CMake и developer tools

Configure всегда создаёт `build/compile_commands.json`. LLVM-style formatter и
clang-tidy запускаются через CMake:

```bash
cmake --build build --target format-check
cmake --build build --target tidy
cmake --build build --target format
```

Полная проверка compiler-изменения:

```bash
scripts/check.sh --with-torch --with-lint
```

Основные build artifacts:

```text
build/lib/CacheScopePassPlugin.so
build/compiler/runtime/libcachescope_trace_runtime.so
build/compile_commands.json
```

Экспериментальные IR, traces и input bundles всегда пишутся в игнорируемый
`output/`, а не смешиваются с CMake build tree.

## Примеры

| Пример | Назначение | Команда |
|---|---|---|
| [hybrid instrumentation](examples/hybrid-instrumentation/README.md) | semantic, fallback, strict failure | `examples/hybrid-instrumentation/run.sh` |
| [live transformations](examples/live-demo/README.md) | baseline/tiling/interchange | `examples/live-demo/run-demo.sh` |
| [manual ABI oracle](examples/manual-instrumentation/README.md) | независимая проверка runtime ABI | `examples/manual-instrumentation/run.sh` |
| [PyTorch demo](examples/torch-demo/README.md) | полный frontend и native execution | `venv/bin/python examples/torch-demo/run.py` |

## Структура

```text
llvm-project/          закреплённый upstream LLVM submodule
compiler/cs-trace/     MLIR pass plugin
compiler/runtime/      runtime только для compiler trace producer
tests/                 CTest integration suites
examples/              воспроизводимые compiler scenarios
output/                локальные результаты экспериментов, ignored
docs/                  архитектура и межкомпонентные контракты
```

Runtime намеренно находится под `compiler/`: он является частью trace producer
и не имеет отношения к runtime будущего cache emulator или UI.

## Документация

- [Архитектура](docs/ARCHITECTURE.md)
- [Командная интеграция для четырёх участников](docs/INTEGRATION.md)
- [Сборка и разработка](docs/DEVELOPMENT.md)
- [Формат trace](docs/TRACE_FORMAT.md)
- [Support matrix](docs/SUPPORT_MATRIX.md)
- [Roadmap](ROADMAP.md)

CacheScope сейчас рассчитан на однопоточные CPU kernels. Это не
cycle-accurate simulator и не предсказатель wall-clock производительности.
