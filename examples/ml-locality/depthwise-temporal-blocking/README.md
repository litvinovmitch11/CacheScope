# Depthwise CNN: layer-by-layer vs temporal blocking

Сценарий сравнивает две реализации одного MobileNet-подобного блока:

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

`temporal-blocked` тайлит выход второй convolution по `10×10`, затем fuse-ит в
внутренний loop её bias initialization, ReLU и первую convolution.
Для tile вычисляется только требуемая область предыдущего слоя с halo.

Обе convolution используют valid boundary policy (`padding=0`). Это сохраняет
stencil/halo-свойство, но не порождает `memref.copy`: semantic policy для
`memref.copy` пока намеренно отсутствует в CacheScope support matrix.

Обе реализации зафиксированы в [`model.py`](model.py): `DepthwiseBlock`
описывает обычное послойное вычисление, а `temporal_blocked_reference`
явно обходит spatial tiles и служит независимой исполняемой спецификацией.
Перед компиляцией они сравниваются bitwise.

В compiler experiment экспортируется канонический `DepthwiseBlock`, поэтому
inputs, веса, исходный MLIR и backend pipeline совпадают. Различие исполняемого
кода целиком задаётся файлами Transform Dialect:

- [`layerwise.transform.mlir`](transforms/layerwise.transform.mlir);
- [`temporal-blocked.transform.mlir`](transforms/temporal-blocked.transform.mlir).

Так сравнивается именно эффект compiler schedule, а не разные kernels или
разная численная аппроксимация.

## Запуск

Нужны собранные зависимости основного проекта и Python environment из
`requirements-torch-demo.lock`:

```bash
venv/bin/python examples/ml-locality/depthwise-temporal-blocking/run.py
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
применяет оба Transform Dialect schedule, создаёт stage dumps и source map,
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
buffer placement для обоих вариантов.

Артефакты записываются в
`output/ml-locality/depthwise-temporal-blocking/`. Числа и интерпретация — в
`RESULTS.md`, машинно-читаемая сводка — в `summary.json`.

Текущий workload создаёт около 177 MiB несжатых traces (примерно 81 MiB для
baseline и 96 MiB для temporal-blocked); `output/` не входит в Git. Standalone
`report.html` занимает около 364 KiB.

## Как читать результат

Ожидается не уменьшение общего числа events: соседние tiles повторно вычисляют
halo. Полезный сигнал — сокращение L1 misses и L2 hits при bitwise-одинаковом
выходе. Число чтений основной памяти может остаться прежним: нижние уровни
иерархии удерживают общий compulsory footprint, который schedule не меняет.

## Контрольный результат

Для seed `19`, tile `10×10` и текущего pipeline получен следующий
детерминированный результат:

| Variant | Events | 16 KiB L1 misses | 32 KiB L1 misses | L2 hits | Memory reads |
|---|---:|---:|---:|---:|---:|
| `baseline` | 581120 | 3314 | 3029 | 1580 | 1449 |
| `temporal-blocked` | 688320 | 1976 | 1450 | 1 | 1449 |

Temporal blocking добавляет 107200 events (`+18.4%`) из-за halo, но сокращает
L1 misses на `40.4%` для 16 KiB и на `52.1%` для 32 KiB. В reference
иерархии это убирает 1579 обращений, которые baseline обслуживал из L2.
Одинаковые 1449 memory reads соответствуют обязательному уникальному
footprint; изменился reuse между L1 и L2, а не набор исходных данных.

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

Standalone `report.html` использует текущий UI и показывает сравнение
вариантов, уровней и instruction statistics. Интерактивный runner уже умеет
пошаговый `step/reset` и чтение sets/ways через HTTP API, но текущий frontend
ещё не визуализирует event timeline и cache state и не синхронизирует две
session. Поэтому этот сценарий пока использует batch replay и static report.

Docker application принимает статический identity-layout `memref<f32>` MLIR
без возвращаемых значений и выполняет один baseline. В образ не входят Torch и
torch-mlir, а generated `output/ml-locality` исключён из build context. Поэтому
PyTorch/Transform Dialect генерация этого сценария остаётся developer flow;
готовый HTML открывается без Docker.
