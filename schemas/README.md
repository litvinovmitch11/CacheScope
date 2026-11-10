# Общие контракты

`cachescope.trace.v1.schema.json` задаёт JSON Schema трассы. Форматы результатов
симуляции описаны в [SIMULATION_FORMAT.md](../docs/SIMULATION_FORMAT.md).

`fixtures/` содержит общие примеры для compiler, runner и UI:

- `trace-v1-*.json` — полная, обрезанная и invalid трассы;
- `source-map-v1/` — трасса со source mapping и соответствующие MLIR artifacts;
- `cache-hierarchy/` — сводка и событийный ряд иерархии для UI.

Ожидаемые отчёты конкретных тестов раннера находятся в `runner/tests/fixtures/`.

Проверка контрактов и source-map artifacts без LLVM:

```bash
python3 tests/schema_suite.py --repo .
python3 scripts/validate_trace.py schemas/fixtures/trace-v1-complete.json
```
