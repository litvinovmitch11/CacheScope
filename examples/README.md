# Examples

Эта директория содержит минимальные входные данные проекта и отдельный
воспроизводимый набор для live demo.

- `matmul.mlir` — минимальный payload MLIR;
- `cache-l1.yaml` — детерминированная L1D model и фиксированное размещение
  буферов;
- `live-demo/` — два исполняемых payload, Transform Dialect strategies,
  lowering до LLVM dialect и проверенный JIT-запуск шести вариантов.

Подробные команды и рекомендуемый сценарий показа находятся в
[`live-demo/README.md`](live-demo/README.md). Примеры проверены на локальной
сборке LLVM/MLIR 24.0.0git из `llvm-project/build`.
