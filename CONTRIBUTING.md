# Contributing

Перед изменением прочитайте:

- [development guide](docs/DEVELOPMENT.md);
- [architecture](docs/ARCHITECTURE.md);
- [support matrix](docs/SUPPORT_MATRIX.md).

Минимальное требование к каждому commit:

```bash
scripts/check.sh --with-lint
git diff --check
```

Изменение канонического pipeline дополнительно проверяется командой:

```bash
python3 examples/trace_demo.py
```

Изменение compiler/runner boundary либо cache model дополнительно требует
низкоуровневый reference flow:

```bash
python3 examples/product-e2e/run.py
```

Изменение Project Studio, experiment API, report или Stepper дополнительно
проверяется на поднятом Docker-приложении:

```bash
docker compose up --build -d
docker compose exec -T cachescope python3 tests/experiment_suite.py \
  --url http://127.0.0.1:9055 --repo .
npm --prefix ui run test:e2e
docker compose down
```

Изменения PyTorch frontend дополнительно требуют:

```bash
scripts/check.sh --with-torch
```

Generated IR, binaries, traces и virtual environments не коммитятся. LLVM/MLIR
берутся из закреплённых apt.llvm.org packages; исходники LLVM не входят в
checkout и не собираются в CI. Новая memory operation принимается только вместе
с точной policy и тестами; best-effort silent skipping не допускается.

Изменение `cachescope.trace.v1` одновременно обновляет JSON Schema,
`scripts/validate_trace.py`, golden fixtures, runtime/schema suites и
`docs/TRACE_FORMAT.md`. Потребители должны продолжать принимать неизвестные
дополнительные поля.
