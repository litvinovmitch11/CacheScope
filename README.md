# CacheScope

CacheScope — локальная лаборатория для низкоуровневой оптимизации Torch-моделей
на CPU. Она позволяет применить несколько MLIR Transform Dialect schedules к
одному блоку нейросети, доказать численную корректность каждого варианта и
сравнить их cache locality на одинаковых моделях кеша.

Основной объект исследования — **compiler schedule**: tiling, fusion,
перестановка циклов и другие преобразования, меняющие порядок обращений к
памяти. CacheScope объясняет эффект преобразования от сводной статистики до
конкретного cache event и породившей его MLIR-операции.

CacheScope не предсказывает wall-clock время и не заменяет Valgrind,
DynamoRIO или аппаратные профилировщики. Это контролируемый эксперимент над
семантической трассой; внешние инструменты используются для независимой
валидации модели и полноты трассировки в явно указанных условиях.

## Быстрый запуск

На машине нужны только Docker и Docker Compose:

```bash
git clone https://github.com/litvinovmitch11/CacheScope.git
cd CacheScope
docker compose up --build
```

После появления статуса `healthy` откройте <http://127.0.0.1:9055>.
Остановка приложения:

```bash
docker compose down
```

Образ содержит UI, backend, LLVM/MLIR 22, Python 3.11 и закреплённые CPU-сборки
Torch/torch-mlir. Compose использует `linux/amd64`: на x86-64 Linux он работает
нативно, а Docker Desktop на ARM запускает его через эмуляцию. Для первой сборки
нужен интернет; рекомендуется не менее 8 GiB памяти и 15 GiB свободного места.

## Основной сценарий в UI

1. На вкладке **«Проект»** нажмите **«Заполнить tile sweep»** или откройте
   папку [`examples/torch-project/depthwise-temporal-blocking`](examples/torch-project/depthwise-temporal-blocking).
2. Отредактируйте Torch-модель, фиксированные inputs, Transform schedules и
   cache profiles. Встроенный пример сравнивает `baseline` и тайлы `6/10/14`.
3. Нажмите **«Запустить проект»**. Каждый schedule независимо проходит export,
   компиляцию, численную проверку, трассировку и cache simulation.
4. Откройте **отчёт**: в нём видны успешные и отклонённые schedules, параметры
   проверки и результаты всех cache profiles.
5. В отчёте выберите конкретные schedule и cache profile, затем откройте
   **Stepper**. Экран показывает выбранную конфигурацию, cache sets/ways,
   hit/miss timeline и переход от события к MLIR source.
6. На вкладке **«Валидация»** доступны методики сравнения с Cachegrind,
   drcachesim и drmemtrace, а также документированные hardware presets.

Дополнительные разделы предназначены для готовых артефактов:

- **MLIR / NPZ** — запуск уже подготовленной MLIR-функции без Torch frontend;
- **Импорт результата** — просмотр существующего experiment bundle;
- **Stepper** — ручная загрузка готовых `trace.json` и MLIR stages.

## Формат редактируемого проекта

Минимальная импортируемая папка:

```text
my-project/
  model.py                 # create_model() -> torch.nn.Module
  inputs.py                # create_inputs() -> tuple[torch.Tensor, ...]
  transforms/
    baseline.mlir          # @__transform_main
    tiled.mlir
  cache-profiles.json      # необязательно
```

Текущая alpha принимает доверенный локальный Python-код, CPU `float32` tensors
с положительными статическими shapes и один Tensor на выходе `forward`.
`create_inputs()` должен вернуть непустой tuple в порядке аргументов модели.
Поддерживаемый набор Torch-операций ограничен экспортом torch-mlir и текущим
MLIR lowering. Точные границы перечислены в
[support matrix](docs/SUPPORT_MATRIX.md).

Для каждого schedule worker сначала вычисляет eager Torch output, затем
сравнивает с ним compiled output через
`torch.testing.assert_close(rtol=..., atol=...)`. Ошибочный schedule остаётся в
отчёте с диагностикой и не участвует в cache comparison; остальные варианты
продолжают выполняться.

`cache-profiles.json` содержит JSON object. Значение профиля — либо массив
коротких строк `NAME:SIZE:LINE:WAYS:POLICY[:SEED]`, либо полная конфигурация
`{ "levels": [...], "hierarchy": {...} }`. Поддержаны LRU, FIFO, Random и
PLRU4, несколько уровней, exclusive/inclusive/NINE, write-back/write-through и
write-allocate/no-write-allocate. Семантика описана в
[моделях кеша](docs/CACHE_MODELS.md).

## Что происходит внутри

```text
Torch model + deterministic inputs
  → eager Torch oracle
  → torch-mlir export в Linalg-on-Tensors
  → отдельный Transform schedule для каждого варианта
  → bufferization и lowering в MemRef/SCF
  → semantic MemRef instrumentation
  → LLVM lowering и fail-closed fallback instrumentation
  → native execution и cachescope.trace.v1
  → проверка полноты trace и source provenance
  → replay каждого trace на одинаковых cache profiles
  → comparative report
  → выбранные schedule/profile в interactive Stepper
```

Инструментация записывает `memref_id`, byte offset, размер и тип доступа, а не
случайный адрес процесса. Runner детерминированно размещает буферы и
воспроизводит одинаковые traces на одинаковых конфигурациях. Отчёт сравнивает
варианты только после numerical correctness gate и сохраняет исходные
параметры эксперимента.

Подробнее: [архитектура](docs/ARCHITECTURE.md),
[формат trace](docs/TRACE_FORMAT.md) и
[source mapping](docs/SOURCE_MAPPING.md).

## CLI и интеграция

Полная инструкция по командной строке находится в
**[docs/CLI.md](docs/CLI.md)**. В ней собраны:

- сборка и прямой запуск MLIR pass plugin;
- compiler → instrumentation → native trace;
- проверка `trace.json`;
- batch cache simulator и многоуровневые конфигурации;
- интерактивный runner HTTP API;
- автономный HTML-отчёт;
- воспроизводимые сравнения с Cachegrind и DynamoRIO.

Минимальный compiler reference flow:

```bash
python3 examples/trace_demo.py
```

Полный низкоуровневый CLI flow с cache replay и автономным отчётом:

```bash
python3 examples/product-e2e/run.py
```

Это developer/reference drivers. Пользовательский продуктовый путь — Docker
Project Studio выше.

## Нативная сборка для разработки

Ubuntu 24.04 и закреплённый LLVM/MLIR:

```bash
scripts/install-llvm-dependencies.sh
cmake -S . -B build -G Ninja \
  -DMLIR_DIR=/usr/lib/llvm-22/lib/cmake/mlir \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
```

Для Torch frontend:

```bash
python3 -m venv venv
venv/bin/pip install -r requirements-torch-demo.lock
scripts/check.sh --with-torch
```

Для запуска полного локального приложения без Docker дополнительно соберите UI
и запустите backend из корня репозитория:

```bash
npm --prefix ui ci
npm --prefix ui run build
build/runner/cachescope_runner_server --app-dir .
```

Затем откройте <http://127.0.0.1:9055>. Полные developer-инструкции находятся
в [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Проверки перед merge

```bash
scripts/check.sh --with-lint
git diff --check
```

Изменения Torch/Project Studio дополнительно требуют `--with-torch` и
Docker/UI E2E. Внешние Cachegrind/DynamoRIO suites запускаются отдельными CI
workflows.

## Карта репозитория

```text
compiler/    MLIR plugin, trace runtime и Torch workers
simulator/   cache levels, policies и hierarchy
runner/      batch CLI, HTTP API и experiment orchestration
ui/          Project Studio, report, Validation и Stepper
schemas/     versioned JSON/OpenAPI contracts и fixtures
examples/    один UI project и поддерживаемые reference drivers
tests/       compiler/runtime/schema/integration suites
docs/        архитектура, CLI, контракты и внешняя валидация
```

Начать дальнейшее чтение: [карта документации](docs/README.md).
