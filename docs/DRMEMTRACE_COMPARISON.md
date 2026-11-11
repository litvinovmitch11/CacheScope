# Сверка semantic trace с машинной трассой drmemtrace

`tests/compare_drmemtrace.py` независимо трассирует выполнение маленького
MLIR-kernel из `tests/fixtures/cache_pipeline.mlir` и сравнивает обращения к
его буферу с semantic trace CacheScope. Проверенный запуск: LLVM/MLIR 22.1.8,
DynamoRIO 11.91.20763, Ubuntu 24.04 AArch64; все шесть обращений совпали по
порядку, read/write, размеру и смещению.

## Запуск

```sh
python3 tests/compare_drmemtrace.py \
  --mlir-bin /usr/lib/llvm-22/bin \
  --plugin build/lib/CacheScopePassPlugin.so \
  --runtime build/compiler/runtime/libcachescope_trace_runtime.so \
  --dynamorio /path/to/DynamoRIO \
  --output-dir /tmp/cachescope-drmemtrace-comparison
```

Нужны собранные plugin и runtime CacheScope, инструменты LLVM/MLIR, `nm` и
DynamoRIO с `drmemtrace`. Для распакованного DynamoRIO может потребоваться
настроить `LD_LIBRARY_PATH`. `--output-dir` должен отсутствовать; если его не
передать, используется временный каталог. В указанном каталоге остаются semantic
trace, полученный LLVM IR, нативный бинарник, сырая трасса и вывод `drmemtrace`,
список нормализованных событий и `comparison.json` с параметрами запуска.

## Методика и границы

1. Существующий driver CacheScope инструментирует MLIR и проверяет semantic
   trace по вручную рассчитанной последовательности из шести событий.
2. Из того же MLIR-файла берётся та же функция kernel, меняется только её
   видимость и добавляется C ABI wrapper. Она отдельно компилируется в нативный
   код без instrumentation CacheScope, с `clang -O0`. Драйвер передаёт ей
   выровненный буфер и проверяет вычисленный результат.
3. `drmemtrace` собирает машинную трассу. Скрипт получает границы функции из
   символов непозиционно-независимого бинарника, а адрес и размер буфера — из
   вывода драйвера. Из полного вывода выбираются обращения инструкций функции
   к этому буферу; адреса нормализуются до смещений относительно его начала.
4. Проверяются одно выполнение функции, завершение потока, границы буфера и
   полный порядок событий `(read/write, byte offset, size)` по отношению к
   semantic trace. При различии скрипт завершится с ошибкой и укажет первое
   расхождение и число событий.

В отбор не входят stack, C ABI wrapper и обращения вне диапазона инструкций
kernel. Поэтому он не доказывает полноту semantic trace для произвольных вызовов
или машинного кода за пределами функции. Эта проверка охватывает только один
однопоточный kernel, один буфер и сборку `-O0`; оптимизации, векторные операции,
другие компиляторы требуют отдельных экспериментов. Matmul рассматривается
отдельно ниже. Трассы выполнены в разных процессах; сопоставляются смещения,
а не фактические адреса процессов.

Workflow `.github/workflows/cache-pipeline.yml` также запускает эту проверку;
результаты и диагностика сохраняются в артефакте `cache-pipeline-results`.

## Matmul: baseline и преобразованные варианты

Отдельный сценарий `tests/compare_drmemtrace_matmul.py` сравнивает semantic и
машинные обращения для `baseline`, `tile4` и `tile4-ikj`:

```sh
python3 tests/compare_drmemtrace_matmul.py \
  --mlir-bin /usr/lib/llvm-22/bin \
  --plugin build/lib/CacheScopePassPlugin.so \
  --runtime build/compiler/runtime/libcachescope_trace_runtime.so \
  --dynamorio /path/to/DynamoRIO \
  --output-dir /tmp/cachescope-drmemtrace-matmul
```

Для каждого варианта нативный kernel создаётся из его `02-loops.mlir` — снимка
после преобразований, но до instrumentation. Скрипт меняет видимость функции и
добавляет C ABI wrapper, затем выполняет lowering и сборку `clang -O0`. Драйвер
передаёт отдельные выровненные буферы A, B и C и проверяет все 256 элементов
результата. Адреса и размеры этих буферов проверяются на выравнивание и
непересечение; машинные обращения внутри функции сопоставляются с `memref_id`
0, 1 и 2, затем сравниваются порядок, тип, размер и смещение каждого события.

На LLVM/MLIR 22.1.8 и DynamoRIO 11.91.20763 для каждого из трёх вариантов
совпали все 16 384 события. Как и для маленького kernel, исключены обращения
вне диапазона инструкций функции, wrapper и stack; проверка относится к одному
потоку и конкретной нативной сборке `-O0`, а не ко всем оптимизациям и CPU.
CI запускает её в workflow `cache-pipeline.yml`; полные трассы и отчёты входят
в артефакт `cache-pipeline-results`.
