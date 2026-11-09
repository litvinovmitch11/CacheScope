# Transform Dialect live demo

Демо показывает, что один payload реально компилируется с разными schedules и
даёт одинаковый математический результат.

## Варианты

Matmul:

- baseline `i → j → k`;
- tile `4×4×4`;
- tile `4×4×4` с interchange `i → k → j`.

Copy:

- baseline row-major;
- tile `4×4`;
- column-first порядок внешних tiles.

## Запуск

```bash
examples/live-demo/run-demo.sh
```

Для внешней сборки:

```bash
MLIR_BIN_DIR=/path/to/llvm-build/bin examples/live-demo/run-demo.sh
```

Результаты находятся в `output/live-demo`:

- `*.transformed.mlir`;
- `*.loops.mlir`;
- `*.llvm.mlir`.

Generated IR не хранится в Git. Этот пример не запускает CacheScope passes;
полный instrumentation execution покрывает CTest `cachescope.end-to-end`.
