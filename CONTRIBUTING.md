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

Изменения PyTorch frontend дополнительно требуют:

```bash
scripts/check.sh --with-torch
```

Generated IR, binaries, traces, virtual environments и LLVM build tree не
коммитятся. LLVM source закреплён как submodule. Новая memory operation
принимается только вместе с точной policy и тестами; best-effort silent
skipping не допускается.

Изменение `cachescope.trace.v1` одновременно обновляет JSON Schema,
`scripts/validate_trace.py`, golden fixtures, runtime/schema suites и
`docs/TRACE_FORMAT.md`. Потребители должны продолжать принимать неизвестные
дополнительные поля.
