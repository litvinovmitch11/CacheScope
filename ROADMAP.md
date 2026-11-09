# CacheScope: roadmap хакатона

Этот документ является главным планом реализации CacheScope на команду из
четырёх человек и три полных рабочих дня. Он отделяет обязательный MVP от
расширенного успешного результата и долгосрочного развития.

Широкое описание продукта и анализ ниши находятся в [README.md](README.md).
Если списки возможностей в README и этом документе различаются, для хакатона
приоритет имеет этот roadmap.

## 1. Формулировка темы

### Смысл проекта простыми словами

Мы берём небольшую программу над массивами — в MVP это умножение матриц — и
компилируем её двумя способами:

1. почти без изменения порядка вычислений (`baseline`);
2. с разбиением вычислений на блоки (`tiled`).

Обе версии считают один и тот же математический результат, но читают элементы
массивов в разном порядке. Во время выполнения мы записываем каждое значимое
обращение к данным: какой массив был затронут, какой элемент читался или
записывался, каков размер доступа и какая MLIR-операция его породила.

Получившуюся последовательность мы воспроизводим в программной модели кеша.
Модель для каждого обращения однозначно вычисляет:

- в какую cache line и set попал адрес;
- был это hit или miss;
- занял ли адрес свободный way;
- какая строка была вытеснена;
- возник ли writeback;
- как после обращения изменилось состояние кеша.

После этого пользователь открывает визуализацию и буквально прокручивает
исполнение по событиям. Он видит рядом MLIR-код, обращение к элементу массива,
состояние sets/ways и накопленные счётчики. Переключаясь между `baseline` и
`tiled`, можно увидеть не только разные итоговые числа, но и сам механизм, из
которого эта разница получилась.

Коротко весь проект выглядит так:

```text
один MLIR kernel
    → две compiler transformations
    → две реальные последовательности обращений к памяти
    → одна и та же формальная модель кеша
    → пошаговая интерактивная визуализация различий
```

CacheScope не пытается угадать, почему программа медленная, не выдаёт советов
по оптимизации и не изображает конкретный CPU. Его задача уже и строже:
показать, как выбранное преобразование MLIR изменило порядок обращений и что
этот порядок делает с явно заданной моделью кеша.

### Короткое название

**CacheScope — интерактивный анализатор влияния MLIR-преобразований на кеш.**

### Формулировка в одно предложение

CacheScope применяет две контролируемые стратегии преобразования к одному MLIR
kernel, строит воспроизводимые traces обращений к памяти, исполняет их на
формально заданной модели L1 data cache и позволяет пошагово увидеть связь
между MLIR access, адресом, cache line, set, way, hit/miss и eviction.

### Проблема

Существующие инструменты хорошо решают отдельные части задачи:

- MLIR Transform Dialect и Polly преобразуют iteration space и memory access
  order;
- Cachegrind, DynamoRIO и Intel Advisor анализируют выполнение готовой
  программы;
- gem5 моделирует детальную микроархитектуру;
- HayStack и родственные работы аналитически считают cache misses для
  ограниченных классов программ;
- compiler pipeline visualizers показывают изменение IR.

Но разработчику компилятора трудно проследить единый путь:

```text
конкретное MLIR-преобразование
  → изменившийся порядок логических обращений
  → адрес и cache line
  → изменение состояния sets/ways
  → итоговые misses и traffic
```

### Ниша

CacheScope — не универсальный profiler и не симулятор конкретного процессора.
Его ниша — **воспроизводимые cache-эксперименты над управляемыми MLIR compiler
transformations**.

### Целевая аудитория

1. разработчики MLIR/LLVM compiler pipelines;
2. разработчики численных и HPC kernels;
3. авторы MLIR dialects и lowering pipelines;
4. преподаватели архитектуры компьютеров и оптимизирующих компиляторов;
5. разработчики, проверяющие cache-locality regressions.

## 2. Что CacheScope обещает и чего не обещает

### Обещание точности

CacheScope даёт точный результат для:

- конкретного сохранённого address trace;
- заданного размещения буферов;
- явно заданной cache model;
- явно заданного initial state.

### CacheScope не утверждает

- что моделирует конкретный Intel, AMD или ARM CPU;
- что предсказывает wall-clock time;
- что число misses совпадёт с PMU реального процессора;
- что выбранная transformation «лучше»;
- что найдено причинное объяснение производительности;
- что UI или LLM дают optimization advice.

### Целевое железо MVP

MVP моделирует **generic single-threaded CPU L1 data cache**:

- один поток;
- один L1D;
- set-associative organization;
- LRU replacement;
- write-back;
- write-allocate;
- cold start;
- normalized virtual model addresses;
- без prefetcher, TLB, coherence и out-of-order effects.

GPU и NPU не входят в хакатонный roadmap.

## 3. Три уровня результата

## 3.1. Железное MVP

Железное MVP — минимальный результат, без которого проект нельзя считать
технически состоявшимся.

### Один kernel

- один подготовленный `linalg.matmul`;
- статическая форма, выбранная так, чтобы trace оставался небольшим;
- `f32`;
- три непересекающихся буфера `A`, `B`, `C`;
- детерминированная инициализация входов.

Рекомендуемый демонстрационный размер: `16×16×16` или `32×32×32`. Cache model
для демонстрации намеренно масштабируется вниз, чтобы на небольшом kernel были
видны capacity и conflict effects. Она должна называться `demo-l1`, а не именем
реального CPU.

### Две реальные MLIR-стратегии

- baseline lowering;
- одна tiled strategy;
- loop interchange только если стабильно поддерживается выбранной LLVM
  revision;
- оба варианта получаются из одного payload;
- результаты вычисления A и B совпадают.

### Реальный trace

Trace должен быть получен из действительно преобразованного и выполненного
MLIR, а не написан вручную.

Каждое событие содержит минимум:

- `sequence`;
- `access_id`;
- `memref_id`;
- логические индексы memref;
- `byte_offset`;
- `size_bytes`;
- `read` или `write`;
- MLIR location;
- variant metadata.

### Одна cache model

- один L1D;
- configurable size, line size и associativity;
- LRU;
- write-back и write-allocate;
- hits, misses, evictions и writebacks;
- обращения, пересекающие границу cache line;
- deterministic result;
- state delta каждого события.

### Минимальный UI

- загрузка двух run files;
- переключение baseline/tiled;
- event slider или кнопки previous/next;
- текущий memory access;
- соответствующая строка MLIR;
- cache line, set и way;
- состояние видимых sets/ways;
- hit/miss/eviction;
- aggregate metrics A/B;
- явное отображение cache configuration.

### Воспроизводимость

Одна команда либо короткая фиксированная последовательность команд должна
полностью пересобирать демонстрацию.

```bash
./cachescope demo
```

или, как допустимый fallback:

```bash
cs-trace ...baseline... -o baseline.trace.json
cs-trace ...tiled...    -o tiled.trace.json
cs-sim baseline.trace.json cache.json -o baseline.run.json
cs-sim tiled.trace.json    cache.json -o tiled.run.json
cs-ui baseline.run.json tiled.run.json
```

### Definition of Done железного MVP

MVP готов, если новый пользователь может:

1. воспроизвести оба варианта из MLIR;
2. проверить эквивалентность результата;
3. получить два настоящих traces;
4. получить два результата cache simulation;
5. выбрать любое отображённое событие;
6. увидеть, какая MLIR access operation породила обращение;
7. увидеть переход состояния cache model;
8. повторить запуск и получить идентичный отчёт.

Если UI работает только на fixture JSON, это UX prototype, но не технически
готовый MVP.

## 3.2. Расширенный успешный вариант

Это результат, который стоит делать только после первого полного прохождения
MVP end-to-end.

### Приоритет 1: синхронный A/B logical diff

- стабильный `logical_key`;
- сопоставление одинаковых логических обращений между вариантами;
- отображение ambiguities и `unmatched`, а не эвристическое угадывание;
- одновременный показ cache outcome A и B;
- изменение reuse distance;
- изменение victim line;
- связь logical access с iteration/tile coordinates.

Для MVP допустимо переключать варианты и сравнивать агрегаты. Полноценное
event-to-event matching — расширенный результат, потому что transformations
меняют порядок повторных обращений и могут создавать one-to-many mapping.

### Приоритет 2: real tile-size phase map

- несколько реально выполненных tile configurations;
- каждая клетка ссылается на сохранённый run;
- failed/illegal transform имеет отдельный статус;
- никаких интерполированных чисел;
- выбор клетки открывает соответствующий replay.

### Приоритет 3: второй kernel

Лучший кандидат — transpose:

- маленький и понятный iteration space;
- визуально очевидно изменение traversal order;
- не содержит matmul-specific reduction;
- доказывает отсутствие жёсткой привязки к одному примеру.

### Приоритет 4: self-contained report

- один каталог или HTML report;
- payload и transformed IR;
- cache configuration;
- trace metadata;
- результаты A/B;
- возможность открыть без backend server.

### Приоритет 5: pass-by-pass view

- payload;
- transformed Linalg;
- lowered loops;
- instrumented IR;
- переход из UI к соответствующей стадии.

### ★ Приоритет 6: аппаратная sanity-check валидация

Это **не часть железного MVP** и не обещание полного совпадения симуляции с
конкретным процессором. Это экспериментальная проверка того, что модель
правильно предсказывает агрегированный эффект transformation на одном доступном
x86-64 Linux CPU.

Для каждого варианта собираются два бинарника из одного transformed MLIR:

```text
transformed MLIR ─┬─ trace build ─────→ trace → CacheScope simulation
                  └─ measurement build → CPU PMU counters
```

Trace build содержит вызовы tracing runtime. Measurement build не содержит
трассировки, потому что она сама меняет cache state. В measurement build
аппаратные counters включаются только вокруг исследуемого kernel через
`perf_event_open`.

Минимальный честный результат:

- сохранить точную модель CPU, ОС, compiler revision и event encoding;
- закрепить выполнение на одном CPU core;
- проверить доступность model-specific L1D events через `perf list`;
- измерять в первую очередь retired loads и L1D load misses;
- отдельно проводить cold и warm эксперименты;
- повторить каждый вариант много раз и показать median и spread;
- сравнить `baseline` и `tiled` по абсолютному miss count и по их отношению;
- явно показать расхождение `simulation ↔ hardware`, а не скрывать его;
- объяснить границы сравнения: prefetcher, stack/spills, runtime accesses,
  lowering, vectorization и начальное состояние cache.

Для первого опыта используются scalar kernels без vectorization/unrolling и
контролируемые аллокации. Перед matmul выполняются три calibration tests:

1. повторный обход working set меньше L1;
2. большой зависимый или псевдослучайный обход;
3. конфликтный доступ к одному набору cache sets.

Критерий успеха — не event-by-event equality. Обычный PMU не предоставляет
полный эталонный журнал hit/miss для каждого memory access. Успех означает, что
CacheScope правильно предсказывает направление и сопоставимый масштаб эффекта
`baseline → tiled`, а наблюдаемое отклонение документировано.

## 3.3. План развития после хакатона

### Ближайшее развитие

- два и более уровня кеша;
- inclusive/exclusive policies;
- PLRU/FIFO/random replacement;
- configurable write policies;
- `affine.for` и static access enumeration;
- vector loads/stores и line splitting;
- dynamic shapes с конкретными runtime values;
- loop fusion/fission;
- padding и strided memref layouts;
- sweep alignment и base offsets;
- compressed/chunked traces;
- CI regression assertions;
- второй и третий kernel.

### Среднесрочное развитие

- C/C++ frontend для ограниченных kernels;
- импорт DynamoRIO `drmemtrace`;
- сравнение с Cachegrind output;
- Cache Bisect по compiler pass pipeline;
- сравнение разных LLVM revisions;
- перенос ★ hardware PMU validation на другие CPU и архитектуры;
- простые next-line и stride prefetchers;
- TLB model.

### Исследовательские направления

- polyhedral/symbolic miss counting;
- интеграция подходов HayStack/Falcon;
- warping cache simulation;
- физические адреса;
- многопоточность и coherence;
- learned replacement models;
- GPU memory transactions и coalescing как отдельный backend.

### Не планируется без отдельного пересмотра продукта

- NPU data-movement simulator;
- cycle-accurate CPU pipeline;
- полноценная замена gem5;
- recommendation engine;
- LLM как источник фактов;
- автоматический optimizer transform schedules.

## 4. Техническая архитектура MVP

```text
payload MLIR + transform A/B
        │
        ▼
┌────────────────────┐
│ cs-trace           │  apply transform, lower, instrument, JIT
└─────────┬──────────┘
          │ trace.v1.json
          ▼
┌────────────────────┐
│ cs-sim             │  deterministic L1D state machine
└─────────┬──────────┘
          │ run.v1.json
          ▼
┌────────────────────┐
│ CacheScope UI      │  replay and A/B overview
└────────────────────┘

tools/cachescope orchestrates the three independent components.
```

В MVP нет общей in-process библиотеки между компонентами. Интеграция идёт
через файлы и CLI exit codes.

## 5. Как именно строится trace

## 5.1. MLIR pipeline

Рекомендуемая вертикаль:

```text
linalg.matmul on memrefs
  → attach CacheScope buffer/access metadata
  → apply baseline or tiled Transform Dialect sequence
  → lower Linalg to SCF/MemRef loops
  → insert CacheScope trace calls around memref accesses
  → lower SCF/CF/Arith/MemRef/Func to LLVM dialect
  → translate to LLVM IR
  → execute through MLIR ExecutionEngine / LLVM ORC JIT
```

Для MVP лучше использовать `linalg.matmul` уже на memrefs, как в
`examples/matmul.mlir`. Это позволяет не включать сложность tensor
bufferization в критический путь.

## 5.2. Transform application

Два transform scripts хранятся отдельно от payload:

- baseline: canonical lowering без tiling;
- tiled: `transform.structured.tile_using_for` с фиксированными sizes;
- interchange задаётся только если синтаксис и семантика выбранной LLVM
  revision подтверждены тестом.

Используется upstream Transform Dialect. Собственный tiling pass в MVP не
пишется.

## 5.3. Instrumentation pass

Собственный pass проходит по поддержанным `memref.load/store` и вставляет
вызовы runtime-функции:

```text
__cachescope_trace(
    sequence,
    access_id,
    memref_id,
    byte_offset,
    size_bytes,
    read_or_write,
    source_location
)
```

Для UI также сохраняются доступные dynamic indices. Полное восстановление
исходных `(i, j, k)` после произвольных transformations не является
обязательным для железного MVP.

## 5.4. JIT execution

- ExecutionEngine/ORC получает lowering до LLVM IR;
- tracing runtime регистрируется как внешний symbol;
- `A`, `B`, `C` инициализируются детерминированно;
- baseline и tiled выполняются на одинаковых данных;
- outputs сравниваются с tolerance, соответствующим `f32`;
- trace считается валидным только после успешной проверки результата.

Instrumentation intentionally влияет на финальную оптимизацию LLVM. MVP
исследует порядок после явных MLIR transformations, а не machine schedule
реального CPU. Это должно быть явно сказано на защите.

## 6. Нормализация адресов

Настоящие pointers JIT нельзя напрямую использовать как model addresses:
allocator и ASLR изменяют mapping в cache sets.

Runtime записывает:

```text
memref_id + byte_offset
```

Simulator строит адрес:

```text
model_address = configured_base[memref_id] + byte_offset
```

Пример:

```yaml
A: 0x10000000
B: 0x20000000
C: 0x30000000
```

Это даёт воспроизводимость и позволяет позднее исследовать alignment. Это не
попытка воспроизвести physical placement реальной ОС.

## 7. Cache simulator

## 7.1. State

```text
Cache
  sets[set_count]
    ways[associativity]
      valid
      dirty
      tag
      last_used
      loaded_by_sequence
```

## 7.2. Address decomposition

```text
line_number = address / line_size
set_index   = line_number % set_count
tag         = line_number / set_count
```

## 7.3. Read

1. найти valid line с совпадающим tag;
2. при совпадении зарегистрировать hit и обновить LRU;
3. иначе выбрать invalid way или LRU victim;
4. при dirty victim зарегистрировать writeback;
5. разместить новую line;
6. зарегистрировать miss и state delta.

## 7.4. Write

1. hit: отметить line dirty и обновить LRU;
2. miss: при write-allocate сначала загрузить line;
3. затем отметить line dirty;
4. при замене dirty victim зарегистрировать writeback.

## 7.5. Line crossing

Обращение `[address, address + size)` разбивается на отдельные line accesses.
Один MLIR access может породить несколько cache outcomes. Связь сохраняется
через parent event ID.

## 7.6. Replay

Каждый simulated event содержит:

- input access;
- outcome;
- selected set/way;
- victim information;
- state delta;
- summary counters after event.

В MVP допустимо хранить все deltas в JSON и восстанавливать состояние с начала
до выбранного события. Checkpoints и inverse deltas нужны позднее.

## 8. Минимальные контракты

## 8.1. `trace.v1.json`

```json
{
  "schema": "cachescope.trace.v1",
  "variant": "tiled-4x4x4",
  "payload_hash": "...",
  "transform_hash": "...",
  "buffers": [
    {
      "id": "B",
      "model_base": 536870912,
      "size_bytes": 1024
    }
  ],
  "events": [
    {
      "sequence": 0,
      "access_id": "matmul.B.read",
      "memref_id": "B",
      "indices": [0, 0],
      "byte_offset": 0,
      "size_bytes": 4,
      "kind": "read",
      "location": "matmul.mlir:8:12"
    }
  ]
}
```

## 8.2. `run.v1.json`

```json
{
  "schema": "cachescope.run.v1",
  "variant": "tiled-4x4x4",
  "cache": {
    "size_bytes": 512,
    "line_size_bytes": 32,
    "associativity": 2,
    "replacement": "lru"
  },
  "summary": {
    "accesses": 16384,
    "hits": 12800,
    "misses": 3584,
    "evictions": 3568,
    "writebacks": 112
  },
  "events": [
    {
      "sequence": 0,
      "access": {},
      "outcomes": [
        {
          "result": "miss",
          "line": 16777216,
          "set": 0,
          "way": 0,
          "evicted": null
        }
      ],
      "state_delta": {}
    }
  ]
}
```

JSON Schema для обоих файлов замораживается на общем стартовом обсуждении.
После первого дня изменения разрешены только для исправления блокирующей
ошибки.

## 9. Разделение на четыре человека

## 9.1. Человек 1: MLIR и trace producer

### Смысл роли

Этот человек отвечает за compiler side проекта. Его задача — доказать, что
`baseline` и `tiled` действительно получены преобразованием MLIR, а события
трассировки действительно вставлены в исполняемую программу, а не нарисованы
заранее.

### Что получает на вход

- исходный `matmul.mlir`;
- зафиксированную LLVM/MLIR revision;
- сигнатуры tracing C ABI от человека 4;
- правила заполнения `access_id`, `buffer_id` и source metadata.

### Железное MVP

1. Зафиксировать рабочую LLVM/MLIR revision и воспроизводимую команду сборки.
2. Запустить исходный `examples/matmul.mlir` и получить правильный результат.
3. Подготовить две реальные transformation strategy:
   - `baseline`, сохраняющий простой порядок циклов;
   - `tiled`, выполняющий настоящее разбиение на блоки.
4. Сохранить IR после каждой важной стадии, чтобы можно было доказать, какое
   преобразование произошло.
5. Понизить оба варианта до уровня `scf`/`memref`, где обращения к памяти уже
   явные и их удобно инструментировать.
6. Реализовать pass, находящий поддерживаемые `memref.load` и `memref.store`.
7. Для каждого такого доступа назначить стабильные `access_id`, `buffer_id`,
   тип доступа, размер и ссылку на исходную MLIR-операцию.
8. Вставить вызов tracing C ABI непосредственно перед доступом к памяти.
9. Сначала проверить pass со stub-функциями, не ожидая runtime человека 4.
10. Затем подключить настоящий tracing runtime к ExecutionEngine или
    скомпилированному executable.
11. Запустить `baseline` и `tiled` на одинаковых входных массивах.
12. Проверить, что обе версии вычисляют эквивалентный результат.
13. Вместе с человеком 4 получить две настоящие трассы, а не fixtures.

### Что отдаёт команде

- исходный, baseline и tiled MLIR;
- команды transformation/lowering/execution;
- instrumentation pass;
- таблицу `access_id → MLIR location/buffer/operation`;
- два исполняемых варианта kernel;
- подтверждение эквивалентности результатов;
- baseline и tiled trace, созданные совместно с runtime.

### Как понять, что MVP роли готов

Если изменить размер матрицы и снова запустить pipeline, трасса должна быть
получена заново из исполнения. Если удалить instrumentation pass, события
должны исчезнуть. Это доказывает, что трасса является результатом реального
compiler pipeline.

### Дополнительные задачи

В порядке приоритета:

1. loop interchange;
2. стабильные logical access IDs;
3. полные loop/tile coordinates;
4. второй kernel — transpose;
5. статический affine trace mode;
6. экспорт стадий lowering для UI.

### Не делает

- cache simulation;
- UI;
- phase map;
- общий CLI.

## 9.2. Человек 2: cache simulator

### Смысл роли

Этот человек пишет формальную и полностью детерминированную модель кеша. Он не
анализирует MLIR и не угадывает причины: он получает готовую последовательность
адресов и строго вычисляет результат каждого обращения по заданным правилам.

### Что получает на вход

- `trace.v1.json` или golden trace того же формата;
- cache configuration: capacity, line size, associativity и policies;
- frozen `run.v1.json` contract.

### Железное MVP

1. Проверять корректность cache configuration: допустимые размеры, число sets
   и согласованность параметров.
2. Для адреса вычислять cache line, set и tag.
3. Хранить для каждого way поля `valid`, `dirty`, `tag` и LRU-порядок.
4. Реализовать load hit и load miss.
5. Реализовать store hit и store miss для политики write-back/write-allocate.
6. На miss выбирать свободный way либо LRU victim.
7. Фиксировать eviction и writeback dirty victim.
8. Корректно разбивать доступ, пересекающий границу cache line.
9. После каждого события сохранять outcome и минимальный `state_delta`,
   необходимый UI для пошагового воспроизведения.
10. Считать агрегаты: accesses, hits, misses, evictions, writebacks и miss rate.
11. Читать `trace.v1.json` и писать валидный `run.v1.json`.
12. Сделать hand-written тесты для hit, compulsory miss, conflict eviction,
    dirty writeback и line crossing.
13. Гарантировать одинаковый `run.v1` при повторном запуске с одинаковыми
    trace, configuration и initial state.

### Что отдаёт команде

- библиотеку cache state machine;
- CLI `cs-sim`;
- набор unit/golden tests;
- `baseline.run.json` и `tiled.run.json`;
- итоговые counters для двух вариантов.

### Как понять, что MVP роли готов

Все заранее рассчитанные golden traces дают ожидаемое состояние sets/ways и
counters. Симулятор не зависит от MLIR и может быть полностью проверен до
появления настоящей трассы.

### Дополнительные задачи

1. checkpoints для быстрого seek;
2. обратный replay;
3. FIFO/PLRU;
4. второй cache level;
5. reuse distance;
6. trace streaming или compression;
7. property tests и differential test с независимым simulator.

### Не делает

- MLIR;
- transformations;
- UI;
- compiler provenance.

## 9.3. Человек 3: UI

### Смысл роли

Этот человек превращает `run.v1.json` в понятный исследовательский интерфейс.
UI ничего не пересчитывает самостоятельно: он честно показывает события и
состояния, которые выдал симулятор.

### Что получает на вход

- frozen `run.v1.json` schema;
- `golden.run.json` в первый день;
- реальные baseline/tiled runs после интеграции;
- MLIR snippets и source locations из metadata.

### Железное MVP

1. Загружать два файла `run.v1.json` и явно сообщать об ошибке формата.
2. Позволять переключаться между `baseline` и `tiled`.
3. Добавить event slider, previous/next и прямой переход к номеру события.
4. Для выбранного события показывать:
   - load или store;
   - имя массива и byte offset;
   - MLIR location и snippet;
   - cache line, set и way;
   - hit, miss, eviction и writeback;
   - victim, если он был.
5. Нарисовать состояние sets/ways и визуально выделять изменившиеся строки.
6. Показать накопленные counters и cache configuration.
7. Сделать понятную легенду цветов, текстовые обозначения и состояния ошибок.
8. Сначала работать с fixture, затем без изменения логики открыть real runs.
9. Проверить, что каждое число в интерфейсе можно найти в загруженном JSON.

### Что отдаёт команде

- запускаемый UI;
- экран пошагового replay;
- переключение вариантов;
- summary comparison;
- понятные empty/loading/error states.

### Как понять, что MVP роли готов

Пользователь без знания внутреннего кода может выбрать событие и ответить на
четыре вопроса: к каким данным обратились, откуда пришло обращение, что сделал
кеш и как изменилось его состояние.

### Дополнительные задачи

1. синхронный A/B split view;
2. logical event matching;
3. iteration-space/tile animation;
4. real phase map;
5. pass-by-pass IR view;
6. self-contained HTML report;
7. filtering и semantic zoom.

### Не делает

- cache calculations;
- MLIR parsing;
- выдуманные UI events;
- optimization recommendations.

## 9.4. Человек 4: trace runtime, execution harness и validation

Это самостоятельный технический поток, а не роль координатора. Человек 1
определяет, **где** в IR возникает событие, а человек 4 отвечает за то, **как**
событие принимается во время исполнения, нормализуется и превращается в
воспроизводимый trace artifact.

Единственный обязательный интерфейс между ними — небольшой frozen C ABI,
например:

```c
void __cachescope_register_buffer(
    uint32_t buffer_id, const void *base, uint64_t size);

void __cachescope_access(
    uint32_t access_id,
    uint32_t buffer_id,
    const void *address,
    uint32_t size,
    uint8_t is_write);
```

Финальная сигнатура фиксируется на kickoff. Человек 1 может тестировать pass со
stub-функциями, а человек 4 — runtime ручными вызовами, не ожидая готового MLIR
pipeline.

### Смысл роли

Instrumentation pass только вставляет вызовы функций. Кто-то должен принять
миллионы таких вызовов во время исполнения, связать реальные указатели с
логическими массивами и сохранить корректную трассу. Это и есть основной
технический результат человека 4. Контракты, единая demo-команда и проверка на
железе естественно находятся рядом с этим runtime, но не заменяют его.

### Что получает на вход

- frozen tracing C ABI;
- список поддерживаемых buffers и их metadata;
- `trace.v1.json` schema;
- instrumented executable или ExecutionEngine callback registration.

### Железное MVP

1. Вместе с командой зафиксировать tracing ABI, JSON Schemas и CLI contracts.
2. Создать hand-written golden trace, вручную рассчитанный golden run и fixture
   для параллельной работы симулятора и UI.
3. Реализовать runtime-функции регистрации buffers и приёма access events.
4. По реальному указателю находить зарегистрированный buffer и вычислять
   `byte_offset`.
5. Назначать каждому buffer детерминированный model base и получать normalized
   address, не зависящий от ASLR конкретного запуска.
6. Присваивать событиям монотонный sequence number.
7. Буферизовать компактные структуры в памяти; не форматировать JSON на каждом
   load/store.
8. Обнаруживать неизвестные buffers, выход за границы и переполнение адреса.
9. После kernel сериализовать события и metadata в валидный `trace.v1.json`.
10. Сохранять shapes, element sizes, model layout, compiler/runtime revisions и
    параметры запуска.
11. Написать standalone runtime test с ручными ABI calls, чтобы не зависеть от
    готовности MLIR pass.
12. Написать contract validator для `trace.v1` и `run.v1`.
13. Сделать execution harness и единую команду `cachescope demo`, связывающую
    compiler, runtime, simulator и UI.
14. Проверить запуск в чистом окружении и собрать воспроизводимый demo bundle.

### Что отдаёт команде

- tracing runtime library;
- standalone runtime tests;
- `trace.v1.json` serializer и validator;
- golden fixtures;
- execution harness;
- единую команду запуска демонстрации;
- metadata, необходимую симулятору и UI.

### Как понять, что MVP роли готов

Ручная тестовая программа без MLIR вызывает tracing ABI и создаёт валидную
трассу. После линковки с человеком 1 тот же runtime без изменений принимает
события instrumented matmul и выдаёт real baseline/tiled traces.

### Дополнительные задачи

1. ★ PMU validation harness через `perf_event_open`;
2. measurement build без tracing callbacks;
3. CPU/event discovery, core pinning и repeated measurements;
4. отчёт `simulation ↔ hardware` с median/spread;
5. chunked/binary trace buffering;
6. tile-size sweep orchestration;
7. report packaging;
8. CI и clean-environment smoke test;
9. benchmark overhead tracing runtime;
10. сравнение с Cachegrind/DynamoRIO на простом trace.

### Не делает

- cache state machine;
- MLIR instrumentation;
- основную реализацию UI;
- не обещает event-by-event совпадение с PMU;
- не подгоняет cache model под один удачный hardware run.

## 10. План на три дня

Команда располагает тремя полными днями разработки и небольшим резервом.
Поэтому план привязан не к часам, а к проверяемым результатам. Первый полный
end-to-end MVP должен появиться за два дня; третий день нельзя заранее отдавать
под обязательную функциональность.

## День 1: независимые работающие компоненты

### Общее стартовое действие

Команда вместе и один раз фиксирует:

- формулировку цели и Definition of Done;
- LLVM/MLIR revision;
- один `matmul` payload;
- baseline и tiled transformation;
- tracing C ABI;
- cache configuration;
- schemas `trace.v1` и `run.v1`;
- CLI names, exit codes и ownership директорий.

После фиксации каждый работает только в своей директории. Изменение контракта
допускается лишь при обнаружении блокирующей ошибки и обсуждается всеми
потребителями этого контракта.

### Параллельная работа

- человек 1 получает baseline/tiled IR, lowering и instrumentation со stub ABI;
- человек 2 прогоняет golden trace через cache state machine и тесты;
- человек 3 открывает golden run и реализует основной event replay;
- человек 4 принимает ручные ABI calls, сохраняет trace и валидирует fixtures.

### Результат дня 1

- каждая из четырёх частей работает отдельно;
- simulator и UI не ждут настоящий MLIR trace;
- compiler pass и runtime не ждут полной интеграции друг с другом;
- есть хотя бы один smoke test на каждый замороженный интерфейс;
- желательно получен первый настоящий trace хотя бы одного варианта.

Если настоящего trace ещё нет, это становится первым общим риском следующего
дня. Новые UI-панели и расширение cache model в этот момент не начинаются.

## День 2: обязательная вертикальная интеграция

Компоненты соединяются в фиксированном порядке:

1. instrumentation pass человека 1 вызывает runtime человека 4;
2. runtime создаёт реальные baseline/tiled `trace.v1.json`;
3. человек 2 воспроизводит эти traces и создаёт `run.v1.json`;
4. человек 3 открывает real runs вместо fixtures;
5. команда проверяет эквивалентность математических результатов;
6. команда повторяет запуск и проверяет детерминизм artifacts.

### Обязательный результат дня 2

Полный путь работает хотя бы документированной последовательностью команд:

```text
MLIR → transform A/B → instrumented execution
     → trace A/B → cache simulation A/B → UI
```

К концу этого этапа пользователь должен уметь открыть настоящее событие,
увидеть соответствующую MLIR operation и переход cache state. Если это не
работает, вся команда продолжает чинить вертикаль; stretch-задачи запрещены.

## День 3: надёжность, красота и расширенный результат

Сначала обязательный MVP доводится до состояния демонстрируемого продукта:

- единая demo-команда или безошибочная документированная последовательность;
- понятные diagnostics для неподдержанных случаев;
- UI показывает только реальные backend data;
- чистый повторный запуск с нуля;
- воспроизводимый README;
- видимые cache configuration и ограничения модели;
- backup bundle с готовыми real artifacts;
- тестовый прогон всей демонстрации без ручного исправления файлов.

После принятия обязательного MVP контракты и основной scope замораживаются.
Участники могут независимо брать дополнительные задачи:

- человек 1: logical access IDs, iteration coordinates или второй kernel;
- человек 2: checkpoints, reuse distance или дополнительная replacement policy;
- человек 3: красивый синхронный A/B view, animation или phase map;
- человек 4: ★ аппаратная PMU-проверка и отчёт `simulation ↔ hardware`.

Небольшой запас после трёх дней используется для исправления blockers,
проверки чистого запуска и упаковки результата. Он не считается временем для
ещё одной обязательной функции.

## 11. Структура репозитория и ownership

```text
CacheScope/
├── schemas/              # человек 4; frozen contracts
│   ├── trace-v1.schema.json
│   └── run-v1.schema.json
├── fixtures/             # человек 4
│   ├── golden.trace.json
│   └── golden.run.json
├── compiler/             # человек 1
│   └── cs-trace/
├── runtime/              # человек 4; trace runtime и execution harness
├── simulator/            # человек 2
│   └── cs-sim/
├── ui/                   # человек 3
├── tools/                # человек 4
│   ├── cachescope
│   └── validate-contract
├── examples/
├── README.md
└── ROADMAP.md
```

До интеграции участники не редактируют директории друг друга. Общего C++/Python
package между четырьмя компонентами в MVP нет.

## 12. Риски и честные fallback-механизмы

### Не работает Transform Dialect integration

Fallback: два заранее полученных transformed MLIR files. Это позволяет показать
trace/simulator/UI, но техническая цель выполнена частично. На защите нельзя
говорить, что CacheScope сам применяет transformations.

### Не работает ORC JIT

Fallback: static enumeration ограниченного affine matmul либо внешний
`mlir-cpu-runner`. Trace всё равно должен выводиться из IR, а не быть написан
вручную.

### Не удаётся восстановить полный logical `(i,j,k)`

Железное MVP показывает `memref_id`, memref indices, byte offset и source
location. Полный event matching между variants переносится в расширенный
результат. Нельзя подменять отсутствующую provenance эвристикой без маркировки.

### Trace слишком большой

- уменьшить shape;
- использовать масштабированную demo cache model;
- установить hard event limit;
- показать `truncated` status;
- не внедрять compression в последний день.

### Simulator не готов полностью

Минимальный fallback: reads + LRU + cold cache. Write policy и dirty writeback
можно временно убрать, но это явно отражается в configuration и pitch.

### UI не готов полностью

Оставить:

- variant switch;
- event slider;
- current access;
- set/way state;
- MLIR location;
- summary metrics.

Iteration animation, split view и phase map удаляются первыми.

## 13. Что показываем на защите

## 13.1. Сценарий показа без привязки ко времени

### Проблема

«MLIR показывает, как компилятор преобразовал kernel. Cache profilers
показывают statistics готового binary. Между transformation и конкретным cache
event теряется проверяемая связь».

### Решение

Показать pipeline:

```text
один payload → baseline/tiled → address traces → одна cache model → visual diff
```

### Live demo

1. открыть baseline;
2. выбрать конкретный `B[k,j]` access;
3. показать address, line, set, way и outcome;
4. перейти к MLIR line;
5. переключить tiled;
6. показать другой порядок событий и агрегированные misses;
7. показать cache configuration и disclaimer.

### Отличие

«Мы не заменяем Cachegrind и gem5. CacheScope занимает промежуток между
compiler transformation и cache experiment: transformation является явной
частью входа, а каждый результат воспроизводим из trace и state machine».

## 13.2. Главный claim

> CacheScope делает compiler transformation первым классом cache-эксперимента
> и сохраняет проверяемый путь от MLIR memory access до конкретного изменения
> состояния cache model.

## 13.3. Чего нельзя говорить

- «мы точнее gem5»;
- «мы моделируем реальный Intel/AMD CPU»;
- «мы нашли причину замедления»;
- «tiled вариант работает быстрее» без аппаратного измерения;
- «мы поддерживаем любой MLIR»;
- «мы заменяем profiler».

Корректно:

- «в заданной модели этот trace даёт N misses»;
- «transform изменила порядок обращений»;
- «для выбранного события line попала в set S и вытеснила way W»;
- «оба variants вычисляют эквивалентный результат».

## 14. Ответы на ожидаемые вопросы

### Почему не Cachegrind?

Cachegrind начинает с готового executable и агрегирует результаты по исходным
строкам/функциям. CacheScope начинает с MLIR payload и transform strategy,
сохраняет compiler context и показывает пошаговое состояние формальной cache
model.

### Почему не gem5?

gem5 моделирует микроархитектуру и timing. CacheScope не моделирует CPU
pipeline; он делает быстрый, воспроизводимый эксперимент над memory access
order, созданным compiler transformation.

### Почему не LLM?

LLM может предложить tile size, но не является исполнителем MLIR и cache state
machine. Все факты CacheScope получаются вычислением и воспроизводятся по
сохранённому trace.

### Для какого железа?

MVP — generic single-threaded CPU-like L1D. Конфигурация явная. Конкретный CPU
не заявляется.

### Это симулятор или profiler?

Trace получается выполнением преобразованного MLIR, а cache outcome — offline
simulation. Поэтому точнее называть CacheScope compiler-aware trace-driven
cache workbench.

### Где польза реальному программисту?

Разработчик MLIR backend сравнивает две transformation strategies до
интеграции в большой benchmark и видит не только разницу counters, но и
конкретные memory accesses и state transitions, из которых она сложилась.

### Где новизна?

Не в новой replacement policy и не в новом tiling algorithm. Отличие — в
интеграции transformation provenance, воспроизводимого access trace и
интерактивного cache replay в одном workflow.

## 15. Финальный checklist перед защитой

### Техническое MVP

- [ ] payload один и неизменный;
- [ ] baseline и tiled реально получены из MLIR pipeline;
- [ ] outputs эквивалентны;
- [ ] traces не являются fixtures;
- [ ] cache model имеет unit tests;
- [ ] cache configuration видна;
- [ ] UI читает real run files;
- [ ] любое отображённое событие присутствует в run JSON;
- [ ] повторный запуск детерминирован;
- [ ] unsupported cases завершаются диагностикой.

### Защита

- [ ] одна чёткая формулировка темы;
- [ ] один главный claim;
- [ ] понятное сравнение с Cachegrind и gem5;
- [ ] disclaimer о model accuracy;
- [ ] live demo проходит без ручного исправления artifacts;
- [ ] backup video/screenshots;
- [ ] README воспроизводим;
- [ ] не заявлены future features как готовые.

## 16. Итоговый приоритет

Порядок ценности на три дня:

1. настоящий MLIR trace;
2. проверенная cache state machine;
3. end-to-end integration;
4. UI, показывающий реальные events;
5. воспроизводимая demo-команда;
6. защита и документация;
7. только затем logical diff, phase map и второй kernel.

Если этот порядок нарушить, команда рискует получить красивый mockup без
технического проекта либо набор backend-компонентов без демонстрируемого
продукта.
