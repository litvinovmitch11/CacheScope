# Общие контракты

`cachescope.trace.v1.schema.json` задаёт JSON Schema трассы. Форматы результатов
симуляции описаны в [SIMULATION_FORMAT.md](../docs/SIMULATION_FORMAT.md).

`cachescope.runner.v1.openapi.json` описывает HTTP API пошагового
runner: [семантика](../docs/RUNNER_API.md),
[примеры запросов и ответов](fixtures/runner-api-v1/README.md).
Существующие файловые форматы trace и отчётов не меняются.

`cachescope.torch-project.v1.schema.json` задаёт редактируемый локальный
Torch/MLIR project: Python entrypoints, варианты Transform Dialect, cache
profiles и параметры validation/trace/Stepper. Cache profile принимает
compact runner-строки либо structured `{levels, hierarchy}` для точных
write/allocation policies и hardware presets.

`fixtures/` содержит общие примеры для compiler, runner и UI:

- `trace-v1-*.json` — полная, обрезанная и invalid трассы;
- `source-map-v1/` — трасса со source mapping и соответствующие MLIR artifacts;
- `cache-hierarchy/` — сводка и событийный ряд иерархии для UI;
- `runner-api-v1/` — HTTP sessions, step/reset, state/statistics и ошибки.

Ожидаемые отчёты конкретных тестов раннера находятся в `runner/tests/fixtures/`.

Проверка контрактов и source-map artifacts без LLVM:

```bash
python3 tests/schema_suite.py --repo .
python3 scripts/validate_trace.py schemas/fixtures/trace-v1-complete.json
```
