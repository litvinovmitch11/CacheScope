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

Изменения PyTorch frontend дополнительно требуют:

```bash
scripts/check.sh --with-torch
```

Generated IR, binaries, traces, virtual environments и LLVM build tree не
коммитятся. LLVM source закреплён как submodule. Новая memory operation
принимается только вместе с точной policy и тестами; best-effort silent
skipping не допускается.
