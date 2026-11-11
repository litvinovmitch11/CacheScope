# Сквозная проверка trace и cache runner

`tests/cache_pipeline_e2e.py` проверяет реальный путь MLIR → instrumentation →
native execution → semantic trace → runner → cache statistics. Он использует
маленький kernel `tests/fixtures/cache_pipeline.mlir` и три стандартных варианта
matmul. Вычисленный результат каждого kernel также проверяется.

Маленький kernel имеет шесть вручную заданных обращений; проверяются их порядок,
операция, memref, смещение и размер, модельный адрес буфера и исходы hit/miss.
Для matmul независимо рассчитывается множество обращений к трем матрицам,
проверяются размеры, количество каждого обращения и различие динамических
порядков baseline/tile/interchange. Для baseline дополнительно проверяется
полный порядок i → j → k, рассчитанный по индексам матриц. Для каждого события runner сверяются
оригинальное событие, адрес из base + offset и результаты; итоговые hit/miss
сравниваются с событийным рядом.

При включённом `CACHESCOPE_BUILD_CACHE_MODEL` тест зарегистрирован в CTest как
`cachescope.cache-pipeline-e2e`. Запуск с существующей сборкой:

```sh
ctest --test-dir build -R ^cachescope.cache-pipeline-e2e --output-on-failure
```

Опционально можно сравнить полученные адреса с drcachesim. Для маленького kernel
сверяется каждый префикс; для вариантов matmul — итоговые hit/miss L1D:

```sh
python3 tests/cache_pipeline_e2e.py \
  --mlir-bin /usr/lib/llvm-22/bin \
  --plugin build/lib/CacheScopePassPlugin.so \
  --runtime build/compiler/runtime/libcachescope_trace_runtime.so \
  --runner build/runner/cachescope-runner \
  --dynamorio-launcher /path/to/DynamoRIO/tools/bin64/drmemtrace_launcher \
  --output-dir /tmp/cachescope-pipeline-validation
```

`--output-dir` должен отсутствовать. В нём сохраняются trace, JSON-отчёты,
бинарные трассы drcachesim и его вывод. Без опций drcachesim и output-dir
проверка не требует внешнего симулятора и использует временный каталог.

Обе модели используют одинаковый L1D и LRU с отключённым prefetch: для
маленького kernel — 128 байт, два way, для matmul — 512 байт, direct-mapped;
размер строки в обоих случаях — 64 байта. Внешняя трасса построена из событий и фактических
модельных адресов runner, а не из машинной трассировки. Поэтому эта проверка
не доказывает эквивалентность semantic trace всем обращениям машинного кода,
не сравнивает dirty writeback и не моделирует реальное железо.

## GitHub Actions

Workflow `.github/workflows/cache-pipeline.yml` запускает CTest и сравнение
с drcachesim на `ubuntu-24.04-arm` при push, pull request и вручную. Он
устанавливает закреплённый LLVM/MLIR и проверяет SHA-256 архива DynamoRIO
11.91.20763. Артефакт `cache-pipeline-results` содержит отчёты и доступную
диагностику, в том числе при ошибке, и хранится семь дней.

Отдельная проверка сверяет semantic trace маленького kernel с независимой
машинной трассой: [DRMEMTRACE_COMPARISON.md](DRMEMTRACE_COMPARISON.md).
