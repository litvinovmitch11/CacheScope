# CacheScope

> Интерактивный cache microscope для LLVM/MLIR: применяет контролируемые
> compiler transformations, строит воспроизводимый trace обращений к памяти и
> показывает, как преобразования меняют состояние формально заданного кеша.

## Статус

Сейчас это спецификация продукта и план реализации. Готового анализатора ещё
нет, поэтому команды целевого `cachescope` CLI ниже пока не реализованы.

При этом compiler-side live demo уже запускается локально: два MLIR payload,
два исходных baseline и четыре transformed-варианта, lowering до явных циклов
и LLVM dialect и JIT-проверка результата. Команда и сценарий показа описаны в
[examples/live-demo/README.md](examples/live-demo/README.md).

Трёхдневный хакатонный объём, разделение работы на четырёх человек и критерии
готовности зафиксированы в [ROADMAP.md](ROADMAP.md). Для определения
обязательного MVP этот roadmap имеет приоритет над широким списком возможностей
ниже.

## Идея в одном абзаце

CacheScope соединяет два обычно раздельных мира: преобразования LLVM/MLIR и
анализ кеша. Пользователь берет один MLIR kernel, применяет к нему две
последовательности Transform Dialect (например, baseline и tiled), запускает их
на одинаковых входных формах и пропускает полученные address traces через одну
и ту же конфигурируемую cache model. Интерфейс связывает `linalg`/`memref`
операцию, логические индексы, динамическое обращение, cache line, set, way,
hit/miss и eviction. Инструмент не дает советов и не пытается угадать причину
замедления: он показывает проверяемые факты эксперимента.

## Проблема

Сегодня нужные части существуют, но находятся в разных инструментах:

- MLIR Transform Dialect и Polly умеют задавать и применять преобразования,
  влияющие на locality;
- Cachegrind, DynamoRIO `drcachesim` и Intel Advisor работают с выполнением
  готовой программы и считают cache-related метрики;
- gem5 моделирует значительно более подробную микроархитектуру;
- HayStack, PolyCache и Falcon аналитически считают misses для ограниченных
  классов программ;
- визуализаторы compiler pipelines показывают изменение IR.

Потерянное звено — интерактивный, compiler-aware эксперимент:

```text
один payload MLIR
        │
        ├── transform script A ── address trace A ──┐
        │                                           ├── cache diff + provenance
        └── transform script B ── address trace B ──┘
```

Обычный cache profiler отвечает: «сколько misses пришлось на эту строку
готовой программы». CacheScope отвечает на другой вопрос: «как конкретная
последовательность MLIR-преобразований изменила порядок логических обращений,
reuse, отображение в sets и жизненный цикл cache lines?»

## Ниша

CacheScope — не универсальный profiler и не hardware performance predictor. Его
ниша — **исследование cache effects управляемых compiler transformations**.

Основные пользователи:

1. разработчики MLIR/LLVM compiler pipelines;
2. разработчики численных и HPC kernels;
3. авторы собственных MLIR dialects и lowering pipelines;
4. преподаватели архитектуры компьютеров и оптимизирующих компиляторов;
5. разработчики, проверяющие cache-locality regression после изменения
   transform script или версии компилятора.

CacheScope особенно полезен, когда программа достаточно структурирована, чтобы
сохранить связь между логическим iteration space и конкретными обращениями:
matrix multiplication, transpose, stencil, convolution, producer/consumer
pipelines, blocked tensor kernels.

## Почему не LLM и не набор правил

CacheScope не формулирует рекомендации вида «используйте tiling 32x32» и не
оценивает код эвристическим score. Его результат вычисляется:

- MLIR действительно преобразуется;
- access trace действительно материализуется для выбранной формы и входа;
- каждое обращение действительно проходит через cache state machine;
- любой hit, miss и eviction можно воспроизвести пошагово;
- агрегированные числа выводятся из тех же событий, которые видны в UI.

LLM может помочь пользователю написать transform script, но не заменяет
CacheScope: она не является исполнителем MLIR semantics, не хранит точное
состояние sets/ways на миллионах обращений и не дает воспроизводимого доказательства
для каждого события.

## Точность: строгий контракт

CacheScope обещает **точный результат для заданного address trace и явно
заданной cache model**. Это не то же самое, что точное предсказание поведения
реального процессора.

Каждый результат обязан хранить:

- версию CacheScope и LLVM/MLIR;
- payload IR и transform IR;
- конкретные shapes и входные параметры;
- способ формирования адресов и base alignment каждого memref;
- полную cache configuration;
- начальное состояние кеша;
- число отброшенных, агрегированных или несопоставленных событий;
- режим trace: static enumeration или instrumented execution.

MVP не моделирует:

- out-of-order execution и speculative loads;
- реальные undocumented replacement policies;
- аппаратные prefetchers;
- TLB и page walks;
- физическое отображение виртуальных страниц;
- coherence и interference других ядер;
- влияние операционной системы;
- wall-clock latency.

Поэтому CacheScope не должен переводить misses в наносекунды и не должен
называть preset конкретного CPU «точной моделью процессора».

## Что мы поддерживаем

### MVP: обязательный объем

#### Вход и преобразования

- [ ] один MLIR module;
- [ ] статические shapes;
- [ ] `linalg` kernels с понижением до `scf`/`memref`;
- [ ] `memref.load` и `memref.store`;
- [ ] upstream MLIR Transform Dialect;
- [ ] минимум две стратегии для одного payload;
- [ ] `tile_using_for`;
- [ ] interchange iterator/loop order;
- [ ] воспроизводимое выделение и alignment буферов;
- [ ] проверка эквивалентности результата A и B на тестовом входе.

#### Trace

- [ ] уникальный `access_id` для логической операции памяти;
- [ ] `memref_id`, индексы, read/write и размер обращения;
- [ ] вычисленный адрес и диапазон затронутых cache lines;
- [ ] loop/tile coordinates;
- [ ] связь с исходным MLIR location;
- [ ] детерминированный порядок событий;
- [ ] лимит trace и явный признак truncation;
- [ ] сериализация запуска в переносимый `.csrun`.

#### Cache model

- [ ] один L1 data cache;
- [ ] конфигурируемые size, line size и associativity;
- [ ] LRU replacement;
- [ ] write-back и write-allocate;
- [ ] cold start;
- [ ] line split для обращений, пересекающих границу строки;
- [ ] hits, compulsory misses, replacements, evictions и writebacks;
- [ ] состояние каждого set/way после любого события;
- [ ] возможность пошагового replay вперед и назад.

#### Сравнение

- [ ] сравнение aggregate metrics A/B;
- [ ] сопоставление логических accesses по provenance, а не по timestamp;
- [ ] изменение reuse distance;
- [ ] изменение числа и типа misses;
- [ ] изменение memory traffic;
- [ ] добавленные и исчезнувшие динамические обращения;
- [ ] доля сопоставленных/несопоставленных событий;
- [ ] отсутствие причинных формулировок, если проведен не контролируемый
  эксперимент.

#### Визуализация

- [ ] payload IR и transform IR A/B;
- [ ] transformation history;
- [ ] iteration-space view;
- [ ] текущий memref access и его логические индексы;
- [ ] cache sets/ways с владельцами линий;
- [ ] hit/miss/eviction timeline;
- [ ] синхронный A/B replay;
- [ ] переход из события к MLIR operation и обратно;
- [ ] tile-size phase map;
- [ ] экспорт воспроизводимого отчета без сервера.

### После MVP: вероятное развитие

- [ ] два и более уровней кеша;
- [ ] inclusive/exclusive/non-inclusive policies;
- [ ] FIFO, PLRU и random replacement;
- [ ] non-write-allocate и write-through;
- [ ] простые next-line/stride prefetchers;
- [ ] `vector.load/store` и `vector.transfer_*`, включая пересечение lines;
- [ ] dynamic shapes с фиксированными значениями конкретного запуска;
- [ ] `affine.for`, `affine.load/store` и static enumeration без JIT;
- [ ] loop fusion/fission, padding и layout transformations;
- [ ] AoS/SoA и произвольные strided memref layouts;
- [ ] sweep base alignment и padding;
- [ ] pass-by-pass Cache Bisect;
- [ ] regression assertions для CI;
- [ ] импорт trace из DynamoRIO `drmemtrace`;
- [ ] импорт агрегированных данных Cachegrind для сверки;
- [ ] C/C++ frontend через Clang/Polygeist для ограниченных kernels;
- [ ] compressed traces и semantic zoom для больших запусков;
- [ ] сравнение разных версий LLVM/MLIR.

### Исследовательские направления, не обещанные продуктом

- [ ] символическое или polyhedral miss counting для больших problem sizes;
- [ ] интеграция аналитической модели наподобие HayStack/Falcon;
- [ ] warping cache simulation;
- [ ] физические адреса и TLB model;
- [ ] многопоточность, private/shared caches и coherence;
- [ ] импорт PMU counters для внешней валидации;
- [ ] learned/undocumented replacement-policy models;
- [ ] автоматический поиск transform schedules;
- [ ] GPU caches и explicitly managed memories.

Последние два пункта особенно важно не затягивать в MVP: autotuning и GPU
уведут проект от ясной ниши.

## Что мы сознательно не делаем

- не диагностируем «проблемы» по набору правил;
- не генерируем optimization advice;
- не ранжируем код субъективным score;
- не используем LLM как источник cache facts;
- не заменяем Cachegrind для произвольных приложений;
- не заменяем gem5 для исследования микроархитектуры;
- не обещаем cycle-accurate или wall-clock prediction;
- не поддерживаем весь LLVM IR в первой версии;
- не скрываем ограничения модели за названием реального CPU.

## Чем CacheScope отличается от существующих инструментов

| Инструмент | Его сильная сторона | Чего от него не получает пользователь CacheScope |
|---|---|---|
| Cachegrind/Callgrind | профиль готового бинарника, статистика по строкам и функциям, diff counters | управление MLIR transformations, iteration-space provenance, пошаговое состояние sets/ways |
| DynamoRIO `drcachesim` | масштабируемый runtime trace, multi-process/thread analysis, configurable simulator, reuse distance | связь одного payload MLIR с несколькими transform scripts и logical-access diff |
| Intel Advisor | зрелый анализ memory access patterns, cache simulation и roofline | открытый MLIR-native transform workbench и воспроизводимый per-event cache replay |
| gem5 | подробная модель CPU, memory hierarchy и timing | быстрый compiler-semantic эксперимент и связь событий с Transform Dialect |
| HayStack/Falcon/PolyCache | быстрый аналитический подсчет misses для affine/static-control программ | динамический lifecycle line/set/way и интерактивное сравнение transform variants |
| Polly | polyhedral analysis и locality transformations | наглядный replay того, как transformation изменила конкретную cache state machine |
| MLIR Transform Dialect | точный контроль transformation sequence | cache experiment и визуальный A/B diff результата transformations |
| Compiler pipeline visualizers | история lowering и IR diff | адреса, sets, ways, reuse и cache-line lifecycle |

Ссылки:

- [MLIR Transform Dialect](https://mlir.llvm.org/docs/Dialects/Transform/)
- [MLIR Affine Dialect](https://mlir.llvm.org/docs/Dialects/Affine/)
- [Polly](https://polly.llvm.org/)
- [Cachegrind](https://valgrind.org/docs/manual/cg-manual.html)
- [DynamoRIO drcachesim](https://dynamorio.org/page_drcachesim.html)
- [gem5 TraceCPU](https://www.gem5.org/documentation/general_docs/cpu_models/TraceCPU)
- [HayStack](https://github.com/spcl/haystack)
- [Intel Advisor Memory Access Patterns](https://www.intel.com/content/www/us/en/docs/advisor/user-guide/2025-0/cachesim-associativity.html)

### Наше преимущество — не «мы во всем лучше»

CacheScope хуже Cachegrind и DynamoRIO по общности входных программ, хуже gem5
по аппаратной детализации и хуже аналитических моделей по работе с огромными
problem sizes. Преимущество существует только в выбранной нише:

> **CacheScope делает compiler transformation объектом cache-эксперимента и
> сохраняет проверяемый путь от Transform Dialect operation до конкретного
> hit/miss/eviction.**

Если продукт не сможет надежно сохранить эту связь, он превратится в еще один
toy cache simulator и потеряет смысл.

## Где идея может развалиться

### 1. Проект останется учебной игрушкой

Поддержка только трех вложенных циклов и матричного умножения недостаточна.
Чтобы выйти из этой ловушки, второй демонстрационный kernel обязан быть не
matmul: transpose или 2D stencil, а третий — producer/consumer с fusion.

### 2. Provenance исчезнет после transformations

MLIR transformations заменяют операции и не обязаны сохранять произвольные
attributes. Нельзя строить продукт только на source locations.

Требование: до преобразования назначать logical access IDs, а при rewrite
собирать relation `origin access -> generated accesses`. Если transformation
не сохраняет relation, UI должен показывать `unmapped`, а не угадывать.

### 3. Trace станет слишком большим

Явное хранение каждого доступа имеет сложность `O(number of accesses)`.

MVP обязан иметь:

- hard limit на число событий;
- явный `truncated` status;
- маленькие демонстрационные shapes;
- агрегаты по tile/loop iteration;
- подготовленную границу для chunked/compressed trace.

### 4. «Точная модель» будет воспринята как реальный CPU

Каждый экран должен показывать текущую модель и режим адресации. Названия
`generic-l1-32k-8way`, а не `intel-core-ultra-exact`.

### 5. Сравнение будет нечестным

Варианты должны использовать одинаковые shapes, layout, base alignment,
initial cache state и семантически одинаковый результат. Иначе diff помечается
как несопоставимый.

### 6. UI станет красивым, но бесполезным

Animation сама по себе не является результатом. Любое визуальное выделение
должно ссылаться на событие с адресом, operation ID и переходом cache state.

### 7. Нас поглотит разработка собственного компилятора

CacheScope должен использовать upstream transformations и стандартное
lowering. Собственные transformations допустимы только как минимальные adapters
для provenance и instrumentation.

## Архитектура

```text
┌───────────────────────────────────────────────────────────────┐
│ Payload MLIR + Transform A/B + shapes + cache.yaml           │
└───────────────────────┬───────────────────────────────────────┘
                        │
                ┌───────▼────────┐
                │ cs-compile     │ apply transforms, lower,
                │                │ attach provenance
                └───────┬────────┘
                        │
                ┌───────▼────────┐
                │ cs-trace       │ static enumeration or
                │                │ instrumented execution
                └───────┬────────┘
                        │ .cstrace
                ┌───────▼────────┐
                │ cs-sim         │ deterministic cache FSM
                └───────┬────────┘
                        │ .csrun A/B
                ┌───────▼────────┐
                │ cs-diff        │ provenance-aware comparison
                └───────┬────────┘
                        │
                ┌───────▼────────┐
                │ CacheScope UI  │ replay, iteration space,
                │                │ sets/ways, phase map
                └────────────────┘
```

### Минимальная схема события

```json
{
  "sequence": 418,
  "variant": "tiled-16x16x8",
  "access_id": "matmul.B.load",
  "memref_id": "B",
  "indices": [8, 21],
  "loop_indices": {"i": 16, "j": 8, "k": 21},
  "kind": "read",
  "virtual_address": 268440912,
  "size_bytes": 4,
  "source_location": "examples/matmul.mlir:8:12"
}
```

Cache outcome хранится отдельно от trace, чтобы один trace можно было replay
через несколько cache configurations.

## Чекпоинты и критерии приемки

### C0 — спецификация эксперимента

- [ ] зафиксирована event schema;
- [ ] зафиксирована cache configuration schema;
- [ ] вручную выписан golden trace на 20–50 обращений;
- [ ] определено, что означает logical access identity;
- [ ] запрещены неявные defaults, влияющие на результат.

**Gate:** два разработчика независимо вычисляют одинаковые ожидаемые cache
states по golden trace.

### C1 — проверенное ядро cache model

- [ ] LRU set-associative model;
- [ ] reads/writes, allocation, eviction, writeback;
- [ ] split access через границу line;
- [ ] unit tests на каждый переход;
- [ ] forward replay и восстановление предыдущего state;
- [ ] детерминированный JSON result.

**Gate:** все hand-written traces совпадают побитно; один trace дает одинаковый
результат на повторных запусках.

### C2 — один MLIR kernel end-to-end

- [ ] `examples/matmul.mlir` проходит upstream pipeline;
- [ ] instrumentation получает адрес, size, logical indices и source location;
- [ ] результат вычисления проверяется эталонной реализацией;
- [ ] trace replay дает видимые hits, misses и evictions.

**Gate:** ни одно событие не существует только в UI — оно восстанавливается из
сохраненного `.csrun`.

### C3 — контролируемый A/B experiment

- [ ] baseline и tiled variants получаются из одного payload;
- [ ] outputs A и B эквивалентны;
- [ ] cache configuration и base addresses идентичны;
- [ ] logical accesses сопоставляются;
- [ ] unmapped events считаются и отображаются.

**Gate:** заранее подготовленная мутация loop order приводит к ожидаемому,
вручную проверяемому изменению trace.

### C4 — минимальный полезный UI

- [ ] IR/transform view;
- [ ] event stepper;
- [ ] iteration-space highlight;
- [ ] cache sets/ways;
- [ ] A/B counters;
- [ ] переход `event -> MLIR location`;
- [ ] состояние читается без animation.

**Gate:** пользователь за пять минут может ответить, какая строка была
вытеснена на выбранном событии и какой MLIR access создал новое обращение.

### C5 — transformation-space map

- [ ] sweep не менее двух tile parameters;
- [ ] каждая клетка карты ссылается на сохраненный run;
- [ ] выбор клетки открывает полноценный replay;
- [ ] failed/illegal transformations отображаются отдельно от измерений.

**Gate:** карта не содержит интерполированных или придуманных значений.

### C6 — три разных демонстрационных kernel

- [ ] matmul: tiling/interchange;
- [ ] transpose или stencil: traversal order;
- [ ] producer/consumer: fusion и lifetime промежуточных данных;
- [ ] автоматический regression test каждого demo.

**Gate:** архитектура не содержит matmul-specific логики.

### C7 — внешняя валидация

- [ ] простые traces сверены с независимым simulator;
- [ ] описаны расхождения с Cachegrind/DynamoRIO и их причины;
- [ ] README не содержит claims, которые не покрыты тестом;
- [ ] зафиксированы ограничения производительности и размера trace.

**Gate:** мы можем объяснить каждое расхождение в терминах различий моделей,
не прибегая к «наш инструмент, вероятно, точнее».

### C8 — релизный MVP

- [ ] одна команда создает self-contained report;
- [ ] все примеры воспроизводятся в чистом окружении;
- [ ] версии LLVM/MLIR зафиксированы;
- [ ] unsupported IR завершается диагностикой, а не частичным результатом;
- [ ] лицензии зависимостей и форматы артефактов документированы.

**Gate:** новый пользователь воспроизводит matmul demo по README без устных
подсказок.

## Пример использования реальным программистом

Елена разрабатывает MLIR pipeline для численного backend. После замены
`tile_sizes [8, 32, 8]` на `[16, 16, 8]` unit tests проходят, но она хочет
понять cache effect до интеграции в большой benchmark. Ей не нужна рекомендация
«верните старый tile»: ей нужен воспроизводимый эксперимент.

### 1. Создать два запуска

```bash
cachescope run examples/matmul.mlir \
  --transform examples/baseline.transform.mlir \
  --cache examples/cache-l1.yaml \
  --output build/baseline.csrun

cachescope run examples/matmul.mlir \
  --transform examples/tiled-16x16x8.transform.mlir \
  --cache examples/cache-l1.yaml \
  --output build/tiled.csrun
```

### 2. Проверить сопоставимость

```bash
cachescope compare build/baseline.csrun build/tiled.csrun \
  --require-equivalent-output \
  --output build/matmul-diff.csreport
```

CacheScope проверяет, что совпадают shapes, cache model, alignment и результат
вычисления. Если нет — diff не объявляется корректным.

### 3. Исследовать результат

```bash
cachescope open build/matmul-diff.csreport
```

Елена выбирает клетку `tile 16x16`, затем access `B[k,j]`. Она видит:

- логические индексы обращения;
- cache line и set;
- содержимое всех ways до и после события;
- какое обращение загрузило вытесненную line;
- reuse distance этой line в обоих вариантах;
- точную MLIR operation и transform stage.

Инструмент не пишет «tile 16x16 плохой». Елена сама решает, приемлемо ли
изменение, запускает дополнительный sweep alignment или выбирает другую
transform strategy.

## Ближайшее решение

Первая версия должна быть не «профилировщиком C++», а небольшим, строгим MLIR
workbench. Главная вертикаль MVP:

```text
linalg.matmul
  → две Transform Dialect стратегии
  → instrumented memref accesses
  → exact model replay
  → provenance-aware visual diff
```

Расширять входной язык можно только после того, как эта вертикаль работает без
скрытых допущений.
