# Depthwise CNN: layer-by-layer vs temporal blocking

Сценарий сравнивает layer-wise baseline и несколько temporal-blocked schedules
одного MobileNet-подобного блока:

```text
output = DWConv3x3(ReLU(DWConv3x3(input)))
```

Форма input activation — `1×2×64×64`, веса обеих convolution — `2×1×3×3`,
а output после двух valid convolution имеет форму `1×2×60×60`. Datatype —
`f32`. Все входы детерминированы seed `19`.

## Основание

Эксперимент адаптирует для ML идею статьи Takayuki Muranushi и Junichiro
Makino, *Optimal Temporal Blocking for Stencil Computation*, Procedia Computer
Science 51 (2015), DOI
[`10.1016/j.procs.2015.05.315`](https://doi.org/10.1016/j.procs.2015.05.315).
Исходная страница статьи:
[ScienceDirect](https://www.sciencedirect.com/science/article/pii/S1877050915011230).

Temporal blocking сокращает memory bandwidth stencil-вычисления, выполняя
несколько зависимых шагов над локальным spatial tile до перехода к следующему
tile. Depthwise `3×3` convolution является channel-wise stencil, а две
последовательные convolution играют роль двух зависимых шагов.

## Сравниваемые реализации

`baseline` сохраняет исходный Torch-граф operator-by-operator. Между слоями
материализуются полные tensors результата первой convolution и ReLU.

`tile-6`, `tile-10` и `tile-14` тайлят выход второй convolution, затем fuse-ят
во внутренний loop её bias initialization, ReLU и первую convolution. Для
каждого tile вычисляется только требуемая область предыдущего слоя с halo.

Обе convolution используют valid boundary policy (`padding=0`). Это сохраняет
stencil/halo-свойство, но не порождает `memref.copy`: semantic policy для
`memref.copy` пока намеренно отсутствует в CacheScope support matrix.

Обе реализации зафиксированы в [`model.py`](model.py): `DepthwiseBlock`
описывает обычное послойное вычисление, а `temporal_blocked_reference`
явно обходит spatial tiles и служит независимой исполняемой спецификацией.
Перед компиляцией они сравниваются bitwise.

В compiler experiment экспортируется канонический `DepthwiseBlock`, поэтому
inputs, веса, исходный MLIR и backend pipeline совпадают. Различие исполняемого
кода целиком задаётся Transform Dialect:

- [`layerwise.transform.mlir`](transforms/layerwise.transform.mlir);
- параметризованным шаблоном
  [`temporal-blocked.transform.mlir.in`](transforms/temporal-blocked.transform.mlir.in).

Так сравнивается именно эффект compiler schedule, а не разные kernels или
разная численная аппроксимация.

## Запуск

Нужны собранные зависимости основного проекта и Python environment из
`requirements-torch-demo.lock`:

```bash
venv/bin/python examples/ml-locality/depthwise-temporal-blocking/run.py
```

По умолчанию запускаются baseline и tiles `6,10,14`. Другой sweep:

```bash
venv/bin/python examples/ml-locality/depthwise-temporal-blocking/run.py \
  --tile-sizes 4,8,12,16
```

Без standalone UI:

```bash
venv/bin/python examples/ml-locality/depthwise-temporal-blocking/run.py --skip-ui
```

Если plugin, trace runtime и runner уже собраны, повторный эксперимент не
требует CMake-конфигурации и сетевого доступа:

```bash
venv/bin/python examples/ml-locality/depthwise-temporal-blocking/run.py \
  --skip-build --skip-ui
```

Driver выполняет Torch reference check, экспортирует Linalg-on-Tensors,
применяет все Transform Dialect schedules, создаёт stage dumps и source map,
инструментирует MemRef/LLVM accesses, выполняет native libraries, валидирует
traces и воспроизводит их на двух cache configurations:

- демонстрационный `16 KiB`, 64-byte line, 4-way LRU L1;
- reference exclusive `32 KiB L1 → 256 KiB L2 → 2 MiB L3`.

Размер задачи выбран так, чтобы полный producer/consumer working set не
помещался в L1, а `10×10` tile с halo помещался. Это необходимая предпосылка
temporal blocking: на искусственно тесном 4 KiB L1 рабочий tile уже не
помещается, и повторное вычисление halo делает schedule хуже baseline.

Tensor-level Linalg не сообщает прямые memory effects, поэтому текущий
`cachescope-assign-origins` запускается после bufferization, но до lowering в
loops. Input и after-transform locations к этому моменту уже сохранены и
переносятся в source map. Общая корректность lineage для произвольного fusion
пока не считается доказанным контрактом. Поэтому driver отдельно требует для
этого сценария одинаковые input fingerprint и origins, а для каждого static
access — одинаковые `access_id`, `memref_id`, kind, `origin_ids` и полный набор
stage locations. Он также требует одинаковые cache configuration и model
buffer placement для всех вариантов.

Артефакты записываются в
`output/ml-locality/depthwise-temporal-blocking/`. Числа и интерпретация — в
`RESULTS.md`, машинно-читаемая сводка — в `summary.json`, готовая
последовательность показа — в `DEMO.md`. `experiment.json` фиксирует версии,
fingerprints, transforms, cache profiles и validation policy.

Sweep `6,10,14` создаёт около 379 MiB несжатых traces; `output/` не входит в
Git. Batch `events.json` по умолчанию удаляются после подсчёта статистики,
поскольку standalone report и interactive stepper используют statistics и
исходный trace. Для диагностики их можно сохранить флагом `--keep-events`.
Standalone `report.html` с четырьмя вариантами и двумя cache profiles занимает
около 516 KiB.

## Как читать результат

Ожидается не уменьшение общего числа events: соседние tiles повторно вычисляют
halo. Полезный сигнал — сокращение L1 misses и L2 hits при bitwise-одинаковом
выходе. Число чтений основной памяти может остаться прежним: нижние уровни
иерархии удерживают общий compulsory footprint, который schedule не меняет.

## Контрольный результат

Для seed `19` и текущего pipeline получен следующий детерминированный sweep:

| Variant | Events | 16 KiB L1 misses | 32 KiB L1 misses | L2 hits | Memory reads |
|---|---:|---:|---:|---:|---:|
| `baseline` | 581120 | 3314 | 3029 | 1580 | 1449 |
| `tile-6` | 785600 | 1982 | 1450 | 1 | 1449 |
| `tile-10` | 688320 | 1976 | 1450 | 1 | 1449 |
| `tile-14` | 665600 | 2275 | 1450 | 1 | 1449 |

Для 16 KiB L1 лучший `tile-10`: он добавляет 107200 events (`+18.4%`) из-за
halo, но сокращает L1 misses на `40.4%`. `tile-6` почти не улучшает этот
результат, зато добавляет 204480 events; `tile-14` уменьшает overhead, но
сокращает misses только на `31.4%`. Для 32 KiB L1 все три tile помещаются и
дают одинаковое сокращение misses на `52.1%`. Одинаковые 1449 memory reads
соответствуют обязательному уникальному footprint; изменился reuse между L1 и
L2, а не набор исходных данных.

## Границы результата

Reference hierarchy использует CPU-подобную геометрию, но фиксированную
exclusive, write-back, write-allocate модель CacheScope. Это не модель
конкретного процессора и не прогноз wall-clock времени: здесь нет hardware
prefetching, speculative execution, coherence и реальной политики LLC.
Результат подтверждает эффект schedule внутри заявленной cache model.

Существующие CacheScope suites независимо сверяют модель с Cachegrind и
drcachesim, а semantic accesses маленького kernel и matmul — с машинной
drmemtrace. Этот depthwise workload пока не входит в drmemtrace/perf suites;
отсутствующий hardware/PMU эксперимент не следует интерпретировать как
провал уже существующей проверки.

## Docker и UI

Standalone `report.html` использует текущий UI и показывает оба cache profiles,
сравнение вариантов, уровней и instruction statistics. Интерактивный frontend
визуализирует event timeline, sets/ways, heatmap и source mapping и работает в
Docker через runner backend. Сценарий автоматически создаёт небольшой
`stepper-demo/`: prefix варианта `tile-10` и все MLIR stages для быстрой живой
демонстрации. Этот prefix не участвует в итоговых метриках; они считаются по
полным traces.

Для живого показа:

```bash
docker compose up --build
```

Откройте `http://localhost:9055/?view=stepper` и выберите все файлы из
`output/ml-locality/depthwise-temporal-blocking/stepper-demo/`. По умолчанию
bundle содержит первые 2048 событий; размер настраивается флагом
`--stepper-events`, значение `0` отключает его создание.

Docker application принимает статический identity-layout `memref<f32>` MLIR
без возвращаемых значений и выполняет один baseline. В образ не входят Torch и
torch-mlir, а generated `output/ml-locality` исключён из build context. Поэтому
PyTorch/Transform Dialect генерация этого сценария остаётся developer flow;
готовый HTML открывается без Docker.
