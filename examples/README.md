# Примеры CacheScope

Основной пользовательский пример находится в
[`torch-project/depthwise-temporal-blocking`](torch-project/depthwise-temporal-blocking).
Его можно открыть из Project Studio: он содержит Torch-модель, фиксированные
inputs, baseline, tile sweep `6/10/14` и cache profiles.

Остальные файлы — поддерживаемые developer/reference flows:

| Путь | Назначение |
|---|---|
| `trace_demo.py` | MLIR → Transform → instrumentation → native trace |
| `product-e2e/run.py` | trace → cache runner → автономный HTML |
| `ml-locality/depthwise-temporal-blocking/run.py` | тот же Torch tile sweep из CLI |
| `ui-upload/` | минимальный fixture для advanced MLIR/NPZ UI и E2E |
| `torch-demo/run.py` | regression-проверка Torch frontend для `scripts/check.sh --with-torch` |
| `a8-l2/run.py` | CI regression для двухуровневого hardware-like cache profile |

## Compiler reference flow

```bash
python3 examples/trace_demo.py
```

Он компилирует один `16×16 f32` matmul как `baseline`, `tile4` и
`tile4-ikj`, инструментирует каждый вариант, проверяет численный результат и
валидирует полные traces. Результат создаётся в `output/trace-demo/` и не
коммитится.

Полная цепочка с batch cache simulation и автономным отчётом:

```bash
python3 examples/product-e2e/run.py
```

Подробные команды для plugin, validator, simulator и server собраны в
[CLI-инструкции](../docs/CLI.md).

Generated IR, traces, reports, binaries, `__pycache__` и virtual environments
не являются примерами и не должны попадать в Git.
