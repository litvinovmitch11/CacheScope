# Cache simulator

Модуль содержит SetAssociativeCacheLevel, политики LRU/FIFO/Random/PLRU4 и
CacheHierarchy. Публичные заголовки находятся в `include/cache_simulator/`,
реализации — в `lib/`, тесты — в `tests/`. LLVM и JSON для сборки не нужны.

CMake предоставляет `cachescope_simulator` для уровней и политик и
`CacheScope::Hierarchy` для иерархии. Раннер подключается отдельно из `runner/`.

## CacheHierarchy

Независимая C++17-библиотека без LLVM и JSON. Владеет уровнями кеша через
`unique_ptr<ICacheLevel>`. Настройки: WB/WT, WA/NWA каждого уровня и
exclusive/inclusive/NINE для соседних пар; defaults — exclusive WB/WA. Строки всех уровней должны иметь одинаковый размер; различающиеся
размеры отклоняются при создании. Конкретные уровни и replacement policies
передаются вызывающим кодом.

```bash
cmake -S simulator -B build/simulator -DCMAKE_BUILD_TYPE=Release
cmake --build build/simulator
ctest --test-dir build/simulator --output-on-failure
```

## Контракт

Используется актуальный [CacheLevel.h](include/cache_simulator/CacheLevel.h)
из `main`. Иерархия линкуется с `cachescope_simulator`; собственного
параллельного API уровня нет. Готовые реализации —
[SetAssociativeCacheLevel](include/cache_simulator/SetAssociativeCacheLevel.h)
и политики [LRU](include/cache_simulator/LRU.h),
[FIFO](include/cache_simulator/FIFO.h) и
[Random](include/cache_simulator/Random.h). Random сбрасывается к заданному seed
при `clear()`, поэтому replay воспроизводим.

- `CacheLine` содержит адрес строки и dirty-флаг. Состоянием владеет уровень.
- `lookup(line_address) const` проверяет наличие без изменения dirty и replacement
  state. Miss не выделяет строку; hit определяется через `LookupResult::hit()`.
- `touch(line_address, type)` обновляет replacement state существующей строки;
  Write также устанавливает dirty. Для отсутствующей строки бросает out_of_range
  без изменения состояния.
- `invalidate(line_address)` удаляет строку и возвращает её вместе с dirty.
- `insert(line)` размещает отсутствующую строку и возвращает set/way и optional
  вытесненную строку, включая её dirty. Вставка обновляет replacement state.
- `clear()` очищает состояние уровня.

`CacheLevel` задаёт имя, `CacheConfig` и владение реализацией. Конфигурация
должна точно соответствовать уровню: capacity, line size, associativity и
replacement policy. Уровень получает только адреса отдельных строк; разбиение
исходного диапазона выполняет иерархия.

## Exclusive по умолчанию

Запрос проверяется от L1 наружу до первого hit. Если строка найдена ниже L1,
она извлекается оттуда и переносится в L1. При полном miss строка загружается
из памяти только в L1. Запись процессора помечает её dirty.

Вытесненная из L1 строка вставляется в L2; вытеснения каскадно передаются дальше.
Dirty сохраняется при каждом переносе. Dirty victim последнего уровня создаёт
memory writeback; clean victim отбрасывается. Финальный flush не выполняется.

`HierarchyAccessResult.lines` содержит только demand probes с 1-based номером
уровня. В default exclusive у lower-level miss `way == nullopt`.
На любом уровне с заполнением way известен после вставки; NWA miss может
оставить null даже у L1. Это наблюдавшийся слот, не финальный snapshot. `evictions` отдельно содержит все
вытеснения, включая каскадные; `memory_reads` и `memory_writebacks` относятся
к обращениям в память. Служебные вставки не увеличивают hit/miss counters.

Иерархия проверяет конфигурацию, границы адресов, set/way, корректность lookup,
invalidate и victim. После ошибки состояние может быть частично изменено;
следующий независимый запуск должен начинаться с `clear()`.

Тесты покрывают переносы между двумя и тремя уровнями, отсутствие дубликатов,
сохранение dirty, запись в память, cross-line access, адрес `UINT64_MAX`,
конфигурацию и обновление LRU при demand hit. Тесты используют реальный
SetAssociativeCacheLevel из `simulator/`, включая его `on_invalidate` для LRU.

## Настройка

`CacheConfig` дополнен `WritePolicy` и `AllocationPolicy` с defaults WB/WA.
Второй необязательный аргумент конструктора — `HierarchyConfig`: вектор
`inclusion_policies` (N−1, пустой = all exclusive), `writeback_clean=false` и
`non_inclusive_fill=true`. API `ICacheLevel` не изменён. Иерархия владеет
политиками межуровневого обмена, уровни по-прежнему владеют dirty и replacement.

`HierarchyAccessResult` также возвращает `back_invalidations`, dirty-передачи
`writebacks`, `memory_writes` и `memory_write_bytes`. Полная семантика и
JSON-конфигурация — [CACHE_MODELS](../docs/CACHE_MODELS.md#политики-записи-и-иерархии).
`cachescope.policies` проверяет worked examples и 2304 сочетания настроек;
прежний exclusive-режим сверяется с независимой эталонной моделью.

## PLRU4

`PLRU4` — Intel486 four-way tree-PLRU из Intel 273021-001, §7.5,
рис. 7-2. Для четырёх ways поддерживает три бита дерева; hit/fill обновляют
путь, invalidate снимает validity. Как и у Intel, свободный way выбирается
в порядке 0,1,2,3 до обращения к дереву. Требует associativity=4.
[Матрица параметров и источники](../docs/PROCESSOR_PRESET_AUDIT.md).
`PLRU4Test` проверяет восемь состояний, отличие от LRU, dirty-victim,
независимость sets, invalidate/free-way и clear.
