# Запуск с разными моделями кеша

В product driver модель одного L1 задаётся четырьмя независимыми
параметрами:

- capacity (`--cache-size-bytes`);
- размер строки (`--line-size-bytes`);
- associativity (`--associativity`);
- replacement policy (`--replacement-policy lru|fifo|random`).

При прямом вызове `build/runner/cachescope-runner` capacity называется
`--size-bytes`; остальные три параметра имеют те же имена.

Иерархия использует exclusive placement, write-back и write-allocate. Эти
политики фиксированы; геометрия и replacement policy каждого уровня
настраиваются отдельно.

## Intel486 DX: приближение по геометрии

```bash
python3 examples/product-e2e/run.py --cache-profile intel-486dx-geometry \
  --output-dir output/product-e2e-486dx

build/runner/cachescope-runner --cache-profile intel-486dx-geometry \
  --trace output/trace-demo/baseline/trace.json \
  --output-dir output/trace-demo/486dx
```

Профиль задаёт L1 8 KiB, строку 16 B и 4-way (128 sets), как у
[Intel486 DX](https://www.ardent-tool.com/CPU/docs/Intel/486/datasheets/240440-006.pdf).
Его нельзя смешивать с отдельными cache-параметрами или `--cache-level`.

**Это приближение по геометрии, не точная модель процессора.** Intel486 DX
использует pseudo-LRU и write-through; CacheScope — LRU и write-back с
write-allocate. Аппаратный L1 объединяет инструкции и данные, а наши трассы
содержат только обращения к данным. Поэтому количество hits/misses и трафик
памяти могут отличаться от реального процессора. Модель адресов также зависит
от размещения буферов в трассе. Метаданные профиля и ограничения сохраняются
в `statistics.json`, `events.json` и `RESULTS.md` product run.

## AMD A8-3850 (Llano): exclusive L1/L2

```bash
python3 examples/product-e2e/run.py --cache-profile amd-a8-3850-lru \
  --output-dir output/product-e2e-a8-3850

build/runner/cachescope-runner --cache-profile amd-a8-3850-lru \
  --trace output/trace-demo/baseline/trace.json \
  --output-dir output/trace-demo/a8-3850
```

Модель одного ядра: L1 data — 64 KiB, 64 B, 2-way LRU; L2 — 1 MiB,
64 B, 16-way LRU. Иерархия exclusive, write-back и write-allocate.
[Руководство AMD для Family 10h/12h](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/software-optimization-guides/40546.pdf)
подтверждает L1 LRU, write-back/write-allocate и exclusive L2;
[замер CPUID для A8-3850](https://valid.x86.fr/ia50qn) подтверждает размеры
и ассоциативности, а [отчёт CPU-Z](https://www.hwupgrade.it/forum/showthread.php?page=178&s=1300ebc54ff000cb5df6afbf8212370e&t=2371637)
указывает строку 64 B для обоих уровней.

Это **приближение**, а не точная модель: политика вытеснения L2 в указанном
руководстве AMD не установлена, поэтому выбрана LRU. В аппаратный L2 попадают
и инструкции; здесь моделируются только данные. Также отсутствуют аппаратный
prefetch, спекулятивные обращения, когерентность и воздействие других ядер;
детали перемещения строк могут отличаться. Ограничения включены в отчёты.
Профиль нельзя смешивать с отдельными cache-параметрами или `--cache-level`.

### Проверка L2 на рабочем наборе 128 KiB

```bash
python3 examples/a8-l2/run.py
```

Пример компилирует и инструментирует MLIR-kernel, дважды читает массив
128 KiB с шагом 64 B и воспроизводит полный trace на `amd-a8-3850-lru`.
Размер массива вдвое больше L1 и помещается в L2. Первые 2048 обращений
заполняют и вытесняют строки; повторный проход даёт L2 hits. Проверяемый
результат для модели CacheScope:

| L1 misses | L1 evictions | L2 hits | L2 misses |
|---:|---:|---:|---:|
| 4096 | 3072 | 2048 | 2048 |

Trace и MLIR-стадии находятся в `output/a8-l2/baseline/`, отчёты — в
`output/a8-l2/simulation/{statistics,events}.json`. Доступны `--llvm-root`,
`--build-dir` и `--output-dir`. Это детерминированная проверка **модели**,
а не измерение аппаратных счётчиков; ограничения профиля действуют и здесь.

## Reference product run

```bash
python3 examples/product-e2e/run.py
```

Дефолтная учебная модель — 512 B, line 64 B, direct-mapped, LRU. Она намеренно
мала для `16×16 f32` matmul: различия порядка обращений дают очевидный cache
effect. Run создаёт `output/product-e2e/RESULTS.md` и `report.html`.

Reference-результат:

| Variant | Hits | Misses | Hit rate | Δ misses |
|---|---:|---:|---:|---:|
| `baseline` | 4096 | 12288 | 25.00% | 0 |
| `tile4` | 7360 | 9024 | 44.92% | -3264 |
| `tile4-ikj` | 7504 | 8880 | 45.80% | -3408 |

Для этой модели меньше misses и больший hit rate означают лучший результат;
`tile4-ikj` — лучший из трёх вариантов. Driver проверяет этот порядок и
завершается с ошибкой при регрессии.

## Геометрия кеша

Пример 1 KiB, 64-byte line, 2-way LRU:

```bash
python3 examples/product-e2e/run.py \
  --output-dir output/product-e2e-1k-lru \
  --cache-size-bytes 1024 \
  --line-size-bytes 64 \
  --associativity 2 \
  --replacement-policy lru
```

Геометрия считается корректной, если:

- capacity, line size и associativity положительны;
- line size — степень двойки;
- capacity делится на `line size × associativity`;
- число sets — степень двойки.

Невалидная геометрия отклоняется до replay.

## Сравнение replacement policies

Policy влияет на результат только при наличии выбора жертвы. Для direct-mapped
cache (`associativity = 1`) LRU, FIFO и Random эквивалентны, поэтому для
сравнения используйте как минимум 2-way модель и разные output directories:

```bash
python3 examples/product-e2e/run.py \
  --output-dir output/product-e2e-lru \
  --cache-size-bytes 512 --line-size-bytes 64 --associativity 2 \
  --replacement-policy lru

python3 examples/product-e2e/run.py \
  --output-dir output/product-e2e-fifo \
  --cache-size-bytes 512 --line-size-bytes 64 --associativity 2 \
  --replacement-policy fifo

python3 examples/product-e2e/run.py \
  --output-dir output/product-e2e-random \
  --cache-size-bytes 512 --line-size-bytes 64 --associativity 2 \
  --replacement-policy random --random-seed 42
```

Проверенный текущий результат для этих команд (число misses):

| Policy | `baseline` | `tile4` | `tile4-ikj` | Лучший variant |
|---|---:|---:|---:|---|
| LRU | 6168 | 3840 | 3648 | `tile4-ikj` |
| FIFO | 6164 | 3716 | 3524 | `tile4-ikj` |
| Random, seed 42 | 5756 | 3805 | 3539 | `tile4-ikj` |

Random полностью воспроизводим для одинакового seed. Seed сохраняется в поле
`cache_config.replacement_policy` статистики. Для честного сравнения меняйте
только один параметр модели за раз.

## Replay существующих traces без повторной компиляции

После `python3 examples/trace_demo.py` можно запускать только simulator:

```bash
build/runner/cachescope-runner \
  --trace output/trace-demo/baseline/trace.json \
  --output-dir output/trace-demo/simulation/baseline-l1-1k-lru \
  --cache-name L1 \
  --size-bytes 1024 \
  --line-size-bytes 64 \
  --associativity 2 \
  --replacement-policy lru
```

Повторите команду для `tile4` и `tile4-ikj`, меняя trace и output directory.
CLI создаёт `statistics.json` и `events.json`.

## Конфигурация, похожая на обычный L1

```bash
python3 examples/product-e2e/run.py \
  --output-dir output/product-e2e-32k \
  --cache-size-bytes 32768 \
  --line-size-bytes 64 \
  --associativity 8 \
  --replacement-policy lru
```

Текущий workload занимает около 3 KiB, поэтому в 32 KiB модели все три
варианта получают одинаковые 48 compulsory misses. Это корректный результат,
но он не демонстрирует пользу tiling. Для hardware-like эксперимента нужен
существенно больший workload либо меньшая модель кеша.

## Многоуровневая модель

`--cache-level NAME:SIZE:LINE:WAYS:POLICY[:SEED]` можно повторить для
L1/L2/L3. Порядок аргументов задаёт порядок проверки от ближайшего к CPU
до самого дальнего. Все уровни должны иметь одинаковый line size.

Учебная маленькая иерархия, в которой видны hits на всех трёх уровнях:

```bash
python3 examples/product-e2e/run.py \
  --output-dir output/product-e2e-l1-l2-l3 \
  --cache-level L1:256:64:1:lru \
  --cache-level L2:512:64:2:lru \
  --cache-level L3:2048:64:4:lru
```

Проверенное число misses:

| Variant | L1 | L2 | L3 | Memory reads | Memory writebacks |
|---|---:|---:|---:|---:|---:|
| `baseline` | 12288 | 4128 | 48 | 48 | 4 |
| `tile4` | 9600 | 648 | 48 | 48 | 4 |
| `tile4-ikj` | 9504 | 168 | 48 | 48 | 4 |

Промах L1 проверяется в L2, а не считается чтением из памяти. Поэтому
суммировать hits/misses разных уровней нельзя. `RESULTS.md` печатает
отдельную таблицу для каждого уровня и отдельно memory traffic.

Точная семантика promotion, evictions и dirty writebacks описана в
[simulator README](../simulator/README.md), а C++ API — в
[runner README](../runner/README.md#api).

## Как читать результаты

- сравнивайте variants только внутри одной и той же полной cache configuration;
- `hits + misses` должно равняться числу line accesses уровня;
- меньший `misses` и больший hit rate лучше для выбранной модели;
- абсолютные результаты simulator не являются прогнозом wall-clock времени;
- одинаковый результат разных schedules может означать, что рабочий набор
  помещается в cache, а не ошибку instrumentation;
- при сравнении проверяйте одинаковое deterministic buffer placement в
  `statistics.json`.
