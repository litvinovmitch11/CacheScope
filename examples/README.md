# Examples

Все примеры используют исходные payload/scripts из Git и пишут результаты
только под игнорируемый `output/`.

| Directory | Что показывает |
|---|---|
| [`hybrid-instrumentation`](hybrid-instrumentation/README.md) | semantic coverage, LLVM fallback и strict rejection |
| [`live-demo`](live-demo/README.md) | Transform Dialect baseline/tiling/interchange |
| [`manual-instrumentation`](manual-instrumentation/README.md) | ручной эталон trace ABI |
| [`torch-demo`](torch-demo/README.md) | PyTorch frontend и native tensor execution |

Для основной автоматической проверки примеры вручную запускать не нужно:

```bash
scripts/check.sh
```

Конфигурации cache в этой директории пока являются fixtures для будущего
simulator и не используются trace producer.
