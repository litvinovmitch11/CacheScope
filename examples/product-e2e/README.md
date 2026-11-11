# Полный продуктовый E2E

[Готовый результат](../../output/product-e2e/README.md) уже сохранён в репозитории.
Скачайте [report.html](../../output/product-e2e/report.html) через **Download raw file**
и откройте его в браузере: Docker, LLVM, Node.js и backend для просмотра не нужны.
Команды ниже нужны только для повторной генерации эксперимента.

Одна команда собирает и соединяет все компоненты CacheScope:

```bash
python3 examples/product-e2e/run.py
```

Сценарий:

1. собирает MLIR plugin, trace runtime и `cachescope-runner`;
2. компилирует и выполняет canonical matmul в вариантах `baseline`, `tile4` и
   `tile4-ikj`;
3. валидирует полные `cachescope.trace.v1` и различие порядков обращений;
4. воспроизводит каждый trace через маленький демонстрационный 512-byte,
   64-byte line, direct-mapped L1 с LRU, write-back и write-allocate;
5. проверяет `statistics.json` и `events.json` на согласованность с trace;
6. собирает автономный HTML-отчёт UI.

Результат находится в `output/product-e2e/`:

```text
summary.json
baseline/{input.mlir,...,trace.json}
tile4/{input.mlir,...,trace.json}
tile4-ikj/{input.mlir,...,trace.json}
simulation/
  baseline-l1/{statistics.json,events.json}
  tile4-l1/{statistics.json,events.json}
  tile4-ikj-l1/{statistics.json,events.json}
RESULTS.md
report.html
```

`RESULTS.md` содержит cache configuration, абсолютные значения, дельты
относительно baseline и явный вывод по минимальному числу misses. HTML показывает
те же сравнения интерактивно.

Для UI нужен Node.js `^20.19.0` или `>=22.12.0` и npm. Если зависимости UI ещё
не установлены, driver автоматически выполняет `npm ci`. Для принудительной
переустановки по lock-файлу передайте `--npm-ci`.

Настройки cache model можно менять явно:

```bash
python3 examples/product-e2e/run.py \
  --cache-size-bytes 65536 \
  --line-size-bytes 64 \
  --associativity 8 \
  --replacement-policy fifo
```

Многоуровневая exclusive hierarchy задаётся повторяемым аргументом:

```bash
python3 examples/product-e2e/run.py \
  --output-dir output/product-e2e-l1-l2-l3 \
  --cache-level L1:256:64:1:lru \
  --cache-level L2:512:64:2:lru \
  --cache-level L3:2048:64:4:lru
```

В таком запуске `RESULTS.md` содержит отдельные таблицы L1/L2/L3 и
memory reads/writebacks. Все уровни должны иметь одинаковый line size.

Дефолтные 512 bytes выбраны намеренно: рабочий набор `16×16` matmul помещается
в обычный 32 KiB L1, и тогда все три варианта получают только одинаковые
compulsory misses. Маленькая учебная конфигурация делает различия порядка
обращений наблюдаемыми и проверяет ожидаемое уменьшение misses в цепочке
`baseline → tile4 → tile4-ikj`. Это демонстрационная модель, а не описание
реального CPU cache.

CLI и driver поддерживают `lru`, `fifo` и детерминированный `random` с
`--random-seed`. Практические команды, ограничения геометрии и правила
сравнения находятся в [CACHE_MODELS.md](../../docs/CACHE_MODELS.md).

Для проверки compiler + simulator без сборки HTML используйте `--skip-ui`.
Пути LLVM, build и output настраиваются через `--llvm-root`, `--build-dir` и
`--output-dir` либо переменные `LLVM_ROOT`, `BUILD_DIR` и
`CACHESCOPE_OUTPUT_DIR`.

Сценарий не является новым форматом данных: он использует существующие
`cachescope.trace.v1`, `cachescope.simulation-statistics.v1` и
`cachescope.simulation-events.v1`. UI по-прежнему можно запускать отдельно над
полученной директорией эксперимента.

Независимая проверка semantic trace, адресов runner и сравнение с drcachesim
описаны в [CACHE_PIPELINE_VALIDATION.md](../../docs/CACHE_PIPELINE_VALIDATION.md).
