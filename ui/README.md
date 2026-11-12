# CacheScope UI

Единое локальное приложение для экспериментов с low-level Torch/MLIR
transformations и cache locality. Production-запуск описан в
[корневом README](../README.md#быстрый-запуск).

## Разделы

- **Проект** — `model.py`, `inputs.py`, Transform Dialect schedules, cache
  profiles, correctness gates и запуск полного pipeline.
- **Валидация** — результаты независимых сравнений с Valgrind Cachegrind,
  DynamoRIO drcachesim/drmemtrace и каталог документированных CPU presets.
- **MLIR / NPZ** — advanced-сценарий для уже подготовленной MLIR-программы.
- **Импорт результата** — read-only просмотр готового experiment bundle.
- **Stepper** — выбранный project schedule/profile либо ручная загрузка
  существующего `trace.json` и MLIR stages. Экран всегда показывает источник,
  variant, profile и фактические levels/hierarchy; interactive session получает
  те же inclusion/writeback/fill policies, что и batch comparison.

После завершения Torch project редактор остаётся отдельным от результатов.
Кнопка **«Открыть отчёт»** открывает comparative report со всеми cache
profiles. Перед Stepper пользователь явно выбирает успешный schedule и cache
profile.

## Запуск

Из корня репозитория:

```bash
docker compose up --build
```

Откройте <http://127.0.0.1:9055>. Образ содержит UI, backend, LLVM toolchain,
Python 3.11 и закреплённые Torch/torch-mlir.

Для разработки UI:

```bash
npm --prefix ui ci
npm --prefix ui run dev
```

Vite проксирует `/api` на `http://127.0.0.1:9055`, поэтому backend должен быть
запущен отдельно.

## Контракт Torch project

Папка для импорта:

```text
my-experiment/
  model.py                    # create_model() -> torch.nn.Module
  inputs.py                   # create_inputs() -> tuple[Tensor, ...]
  cache-profiles.json         # необязательно
  transforms/
    baseline.mlir
    tile-6.mlir
    tile-10.mlir
    tile-14.mlir
```

`cache-profiles.json` — JSON object. Значение профиля может быть:

- массивом коротких строк `NAME:SIZE:LINE:WAYS:POLICY[:SEED]`;
- точной конфигурацией `{ "levels": [...], "hierarchy": {...} }` с
  write/allocation policies.

Studio умеет добавить два проверенных hardware presets: IntelDX2 L1 WT и
Write-Back Enhanced IntelDX4 L1 WB. Они передаются worker’у как полные
конфигурации, а не как декоративные названия. Границы и источники перечислены в
[аудите presets](../docs/PROCESSOR_PRESET_AUDIT.md).

Готовый пример:
[`examples/torch-project/depthwise-temporal-blocking`](../examples/torch-project/depthwise-temporal-blocking).
Он содержит baseline и tile sweep `6/10/14`. Worker получает один eager Torch
output и отдельно проверяет против него результат каждого compiled schedule
через `torch.testing.assert_close(rtol, atol)`; failed schedule не участвует в
cache comparison, но остаётся в отчёте с диагностикой.

## Контракт MLIR / NPZ

Advanced-вкладка не принимает Torch project. Ей нужны уже подготовленная
MLIR-функция со статическими `memref<...xf32>` аргументами и NumPy-архив:

```python
import numpy as np
np.savez(
    "inputs.npz",
    image=np.zeros((1, 3, 224, 224), dtype=np.float32),
)
```

`image` — имя массива, которое появится в UI. После чтения файлов пользователь
явно связывает каждый аргумент функции с NPZ-массивом той же формы и dtype либо
выбирает нулевой буфер. «Подходящая форма» означает точное равенство всех
статических dimensions, а не автоматический reshape или broadcasting.

## Внешняя валидация

UI показывает только воспроизводимые результаты существующих suites:

- Cachegrind: 25/25 совпавших префиксов;
- drcachesim: 19/45/49 совпавших префиксов для L1D и двух-/трёхуровневой
  exclusive hierarchy;
- drmemtrace: 6 событий малого kernel и 16 384 события каждого из трёх matmul
  variants совпали с semantic trace.

Это независимая проверка модели и машинной трассы, а не PMU-замер и не прогноз
wall-clock performance. Методики: [Cachegrind](../docs/CACHEGRIND_COMPARISON.md),
[drcachesim](../docs/DRCACHESIM_COMPARISON.md),
[drmemtrace](../docs/DRMEMTRACE_COMPARISON.md).

## Автономный отчёт

Готовую experiment directory можно упаковать в один HTML:

```bash
npm --prefix ui run report -- \
  --input ../output/experiment \
  --output ../reports/experiment.html
```

Torch Project Studio использует основной interactive report screen, потому что
ему нужны backend artifacts и переход в выбранный Stepper.

## Проверки

```bash
npm --prefix ui run build
npm --prefix ui run lint
npm --prefix ui run test:e2e
```

Полный E2E требует запущенного приложения. Другой адрес задаётся через
`CACHESCOPE_E2E_URL`.

## Границы alpha

- trusted local Python, без multi-user sandbox;
- CPU `float32`, static positive shapes, один Tensor на выходе;
- один текущий experiment хранится во временной директории backend;
- контролируемая cache model, не cycle-accurate CPU simulation;
- standalone HTML не содержит Stepper и запуск проекта.
