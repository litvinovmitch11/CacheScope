# Local application architecture

CacheScope работает как локальное Docker-приложение, а не hosted code runner.
Python пользователя считается доверенным локальным кодом.

```text
Project Studio
  model.py + inputs.py + transforms/*.mlir + cache profiles
      │ POST cachescope.torch-project.v1
      ▼
Runner HTTP server
  job lifecycle / progress / cancellation / artifacts
      │ separate process
      ▼
Torch project worker
  eager oracle → torch-mlir export → schedule compilation
  → numerical/provenance validation → semantic trace
  → replay through every cache profile
      │
      ├─ ui-results.json → comparative report
      └─ stepper/<schedule>/ → selected interactive Stepper
```

## UI routes

| Route | Назначение |
|---|---|
| `/` | основной Torch Project Studio |
| `/?view=report&experiment=<id>` | comparative report текущего experiment |
| `/?view=stepper&experiment=<id>&variant=<name>&profile=<name>` | Stepper выбранного schedule/profile |
| `/?view=validation` | внешняя валидация и hardware presets |
| `/?view=mlir` | advanced MLIR/NPZ pipeline |
| `/?view=import` | read-only импорт result bundle |
| `/?view=stepper` | ручная загрузка trace/stages |

## Project contract

- `model.py`: `create_model() -> torch.nn.Module`;
- `inputs.py`: `create_inputs() -> tuple[torch.Tensor, ...]`;
- каждый schedule содержит `transform.named_sequence @__transform_main`;
- compact cache profile — массив runner-строк;
- structured cache profile — `{levels, hierarchy}` с полными write/allocation
  policies;
- settings задают tolerances, trace limit, Stepper prefix и timeout.

Wire format: [cachescope.torch-project.v1](../schemas/cachescope.torch-project.v1.schema.json).

## Job and artifacts

Server хранит один текущий job во временной директории и запускает worker в
отдельной process group. Это обеспечивает cancel, timeout и изоляцию падения,
но не является security sandbox.

Успешный project публикует:

```text
output/
  <schedule>/trace.json + MLIR stages
  simulation/<profile>--<schedule>/statistics.json
  simulation/<profile>--<schedule>.config.json   # structured profiles
  summary.json
  ui-results.json
  stepper/<schedule>/trace.json + MLIR stages
```

Report и Stepper читают эти artifacts через same-origin API. Для каждого
успешного schedule создаётся отдельный Stepper bundle; UI не выбирает скрытый
preferred-вариант. Stepper manifest возвращает фактически выбранные `variant`
и `profile`; экран сохраняет их над сессией вместе с полной геометрией уровней,
в том числе если endpoint применил default из-за неполной ссылки.

## Validation boundary

Вкладка Validation показывает результаты существующих воспроизводимых suites:
Cachegrind, drcachesim и drmemtrace. Они подтверждают cache transitions и
соответствие semantic/machine traces в документированных условиях. Это не
сравнение wall-clock времени с физическим CPU.

Hardware presets содержат только параметры с источниками для каждого
показываемого поля. Studio передаёт их worker’у как structured profiles.

## Alpha limitations

- CPU, `float32`, static positive shapes, один Tensor output;
- один ephemeral job без истории после перезапуска;
- базовый textarea вместо полноценного IDE editor;
- universal worker пока повторно использует orchestration-функции canonical
  depthwise driver;
- пользовательский Python исполняется без sandbox как локальный код владельца.
