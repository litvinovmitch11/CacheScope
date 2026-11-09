# Manual instrumentation ABI oracle

Этот пример содержит вручную вставленные `__cachescope_trace_v1` calls. Он
нужен как маленький независимый эталон ABI и serializer, если output
автоматического pass вызывает сомнения.

```bash
examples/manual-instrumentation/run.sh
```

Pipeline:

```text
hand-instrumented MemRef MLIR
  → LLVM dialect
  → mlir-runner + shared runtime
  → JSON trace
  → deterministic verification
```

Ожидается `16 384` kernel events. Initialization и result checking не
трассируются. Артефакты находятся в `output/manual-instrumentation`.

Новая функциональность должна реализовываться в автоматическом plugin; ручной
пример не является вторым production pipeline.
