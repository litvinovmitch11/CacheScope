# ML locality experiments

Эта серия сравнивает попарно две реализации одной ML-функции. В каждой паре
совпадают входы, веса, результат, backend flags и cache hierarchy; меняются
только schedule и материализация промежуточных тензоров.

План:

1. stacked depthwise CNN: layer-by-layer против temporal blocking;
2. Transformer MLP: materialized expanded hidden tensor против tiled fusion;
3. EmbeddingBag: materialized gather против fused gather-reduce.

Первый реализованный сценарий находится в
[`depthwise-temporal-blocking`](depthwise-temporal-blocking/README.md).

Эти сценарии сравнивают compiler schedules внутри явно заданной CacheScope
model. CPU-подобная геометрия не означает hardware/PMU validation и не
предсказывает wall-clock performance. Внешние проверки Cachegrind, drcachesim
и drmemtrace имеют отдельные, более узкие условия в `docs/`.
