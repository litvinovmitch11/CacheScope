# PyTorch → MLIR → native trace

Это дополнительная frontend-интеграция. Для основного compiler/trace пути без
PyTorch используйте [`../trace_demo.py`](../trace_demo.py); он быстрее,
самодостаточнее и запускается в обязательном end-to-end test.

Демо принимает обычный `torch.nn.Module`, экспортирует его в
Linalg-on-Tensors и строит три варианта одного `forward`:

- `baseline`;
- `tile4`;
- `tile4-ikj`.

Каждый вариант bufferize/lower/instrument/compile выполняется отдельно, но на
одинаковых input tensors. Результат сверяется с PyTorch.

## Запуск

```bash
venv/bin/python examples/torch-demo/run.py
```

Либо вместе со всеми проверками:

```bash
scripts/check.sh --with-torch
```

Нужны `torch`, `torch-mlir` и `numpy`. Проверенные версии находятся в
[`requirements-torch-demo.lock`](../../requirements-torch-demo.lock).

## Результаты

`output/torch-demo` содержит input manifest, `inputs.npz`, IR всех стадий,
object/shared libraries и traces.
`comparison.json` показывает для каждого варианта:

- total/recorded events;
- semantic event count;
- LLVM fallback event count;
- `valid`/`complete` trace status;
- первую разницу dynamic order для каждой пары variants.

В текущем matmul все `16 640` events semantic и fallback count равен нулю.

## Ограничения

- shape зафиксирован как `16×16`;
- dtype — `f32`;
- wrapper рассчитан на два inputs и один output;
- это прозрачный AOT demo, а не `torch.compile` backend.
