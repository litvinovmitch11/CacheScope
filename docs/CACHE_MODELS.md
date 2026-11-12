# Запуск с разными моделями кеша

В product driver модель одного L1 задаётся четырьмя независимыми
параметрами:

- capacity (`--cache-size-bytes`);
- размер строки (`--line-size-bytes`);
- associativity (`--associativity`);
- replacement policy (`--replacement-policy lru|fifo|random|plru4`).

При прямом вызове `build/runner/cachescope-runner` capacity называется
`--size-bytes`; остальные три параметра имеют те же имена.

По умолчанию иерархия использует exclusive, write-back и write-allocate.
UI, HTTP API и CLI `--config` поддерживают настройки, описанные ниже.
Короткие CLI-флаги и product driver сохраняют прежние defaults.

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
CLI-профиль нельзя смешивать с `--config`, отдельными cache-параметрами или
`--cache-level`. Это отдельное приближение CLI; в список подтверждённых
процессоров UI оно не включено.

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
CLI-профиль нельзя смешивать с `--config`, отдельными cache-параметрами или
`--cache-level`; он не входит в список подтверждённых процессоров UI.

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

## Политики записи и иерархии

CLI `--config file.json`, POST `/api/v1/sessions` и запуск эксперимента из UI
принимают одинаковые `levels` и необязательный `hierarchy`. Пример JSON
конфигурации для CLI (в HTTP добавьте `trace_path`):

```json
{
  "levels": [
    {"name":"L1", "size_bytes":32768, "line_size_bytes":64,
     "associativity":8, "replacement_policy":"LRU",
     "write_policy":"write_through", "allocation_policy":"no_write_allocate"},
    {"name":"L2", "size_bytes":262144, "line_size_bytes":64,
     "associativity":8, "replacement_policy":"FIFO",
     "write_policy":"write_back", "allocation_policy":"write_allocate"}
  ],
  "hierarchy": {
    "inclusion_policies":["inclusive"],
    "writeback_clean":false,
    "non_inclusive_fill":"all_levels"
  }
}
```

```bash
build/runner/cachescope-runner --config /tmp/cache-config.json \
  --trace output/trace-demo/baseline/trace.json --output-dir output/policy-replay
```

`write_policy` каждого уровня: `write_back` (default) удерживает dirty до
передачи вниз; `write_through` передаёт каждую CPU-запись дальше, сохраняя
чистую локальную копию. `allocation_policy`: `write_allocate` (default)
запрашивает строку при промахе CPU-записи; `no_write_allocate` передаёт запись
дальше без заполнения этого уровня. Read miss всегда заполняет запрашиваемый
уровень. Полные служебные writebacks и обязательные inclusive-копии размещаются
независимо от CPU no-write-allocate; частичная запись не теряется.

`inclusion_policies` содержит N−1 значений для N уровней, начиная с L1/L2:

- `exclusive`: непрерывный exclusive-сегмент хранит одну копию строки;
  promotion извлекает исходную строку, все victims передаются вниз;
- `inclusive`: каждая строка верхнего уровня присутствует в нижнем;
  lower eviction инвалидирует верхние копии и собирает их dirty-данные;
- `non_inclusive` (NINE): дубли разрешены, lower eviction не инвалидирует
  верхние копии. `non_inclusive_fill="all_levels"` заполняет уровни до найденной
  строки/памяти, пока не встретит exclusive-связь. `"on_eviction"` заполняет
  только запрашиваемый уровень; нижний получает victims.

Можно смешивать политики соседних пар. `writeback_clean=false` (default)
отбрасывает чистые victims на non-exclusive связях; `true` передаёт их ниже.
Dirty victims передаются всегда. WT-запись обходит уровни exclusive-сегмента,
если размещение там создало бы вторую копию. Передача dirty-линии в WT-кэш
немедленно отправляет её дальше; WT-строки остаются чистыми.

Defaults без новых полей: все связи exclusive, каждый уровень WB/WA.
Размеры строк всех уровней должны совпадать. Prefetch, когерентность, задержки,
write-combining и финальный flush не моделируются.

`memory.writes` считает все записи в память; `writebacks` — их подмножество с
полной dirty-строкой. `write_bytes` считает переданные байты: размер строки
для dirty writeback и фактическую часть CPU-записи для WT/NWA. Событие через
границу строки создаёт отдельную транзакцию на каждую часть. `totals.writebacks`
считает dirty-передачи уровня, включая обратную инвалидацию или слив dirty
при promotion в WT; он может превышать `evictions`. `back_invalidations`
считает удалённые inclusive-копии отдельно от capacity evictions. Служебный
трафик не добавляет hit/miss: один demand probe на пару (уровень, строка)
за одно исходное событие, классификация read/write — по исходному запросу.

## Документированные профили CPU

UI включает IntelDX2 (L1 WT) и Write-Back Enhanced IntelDX4 (L1 WB).
Геометрия, политики и условия режима подтверждены руководством Intel;
[матрица параметров и ссылки](PROCESSOR_PRESET_AUDIT.md).
PLRU4 — дерево четырёх ways, не замена на LRU. CLI/driver принимают
`--replacement-policy plru4 --associativity 4`; для задания WT/NWA или WB/NWA
передайте политики в `--config` или запустите профиль из UI.
