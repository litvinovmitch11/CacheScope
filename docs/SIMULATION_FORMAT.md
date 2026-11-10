# Формат результатов симуляции: `cachescope.simulation-statistics.v1`

Симулятор кэша принимает `cachescope.trace.v1` и конфигурацию кэша, выполняет симуляцию и сохраняет результаты отдельно от исходной trace.

Для каждого эксперимента, конфигурации кэша и варианта создаётся отдельная директория:

```text
output/<experiment>/
  simulation/
    <config-name>/
      statistics.json
      events.json
```

- `statistics.json` — обязательная сводная статистика.
- `events.json` — необязательные результаты отдельных обращений к кэшу.

Все пути к файлам указываются относительно `output/<experiment>/`.

## 1. Статистика

`statistics.json` содержит итоговые результаты одного запуска симуляции.

```json
{
  "schema": "cachescope.simulation-statistics.v1",
  "variant": "baseline",
  "producer": "instrumented-mlir-execution",

  "cache_config": {
    "size_bytes": 32768,
    "line_size_bytes": 64,
    "associativity": 8,
    "set_count": 64,
    "replacement_policy": "LRU"
  },

  "processed": {
    "trace_events": 10000,
    "line_accesses": 18000
  },

  "totals": {
    "read_line_accesses": 12000,
    "write_line_accesses": 6000,
    "hits": 12000,
    "misses": 6000,
    "read_hits": 8000,
    "read_misses": 4000,
    "write_hits": 4000,
    "write_misses": 2000,
    "evictions": 400  
  }
}
```

Значения в примере иллюстративные.

### Header

| Поле | Тип | Описание |
|---|---|---|
| `schema` | string | версия формата |
| `variant` | string | вариант, например `baseline` или `tile4` |
| `producer` | string | источник trace |
| `cache_config` | object | конфигурация кэша |
| `processed` | object | количество обработанных событий |
| `totals` | object | итоговые счётчики |

### Cache configuration

| Поле | Тип | Описание |
|---|---|---|
| `size_bytes` | integer | общая ёмкость кэша в байтах |
| `line_size_bytes` | integer | размер строки кэша |
| `associativity` | integer | количество ways в одном set |
| `set_count` | integer | количество наборов |
| `replacement_policy` | string | политика замещения, например `LRU` |

Указываются фактические значения конфигурации, использованной при симуляции.

### Processed

| Поле | Тип | Описание |
|---|---|---|
| `trace_events` | integer | количество обработанных событий исходной trace |
| `line_accesses` | integer | общее количество обращений к строкам кэша |

Одно событие trace может затрагивать несколько строк кэша, если диапазон адресов пересекает границу cache line.

### Totals

| Поле | Тип | Описание |
|---|---|---|
| `read_line_accesses` | integer | количество обращений к строкам кэша для чтения |
| `write_line_accesses` | integer | количество обращений к строкам кэша для записи |
| `hits` | integer | общее количество попаданий |
| `misses` | integer | общее количество промахов |
| `read_hits` | integer | попадания при чтении |
| `read_misses` | integer | промахи при чтении |
| `write_hits` | integer | попадания при записи |
| `write_misses` | integer | промахи при записи |
| `evictions` | integer | количество вытесненных строк кэша |

Все счётчики hits и misses рассчитываются на уровне строк кэша, а не исходных событий trace.

Обязательные соотношения:

```text
hits + misses = line_accesses

read_line_accesses + write_line_accesses = line_accesses

read_hits + write_hits = hits

read_misses + write_misses = misses
```

`evictions` — количество вытеснений существующих строк. Промах не обязательно приводит к вытеснению: например, если в наборе есть свободный way.

### Rates

UI самостоятельно рассчитывает:

```text
hit_rate  = hits / line_accesses
miss_rate = misses / line_accesses
```

Результаты отображаются в процентах.

Если `line_accesses == 0`, вместо значения показывается `—`.

## 2. События (опционально)

`events.json` содержит результаты отдельных обращений к кэшу. Файл необязателен и не требуется для отображения основной статистики.

```json
{
  "schema": "cachescope.simulation-events.v1",
  "variant": "baseline",
  "events": [
    {
      "sequence": 12,
      "kind": "read",
      "address": "0x103c",
      "size_bytes": 8,
      "lines": [
        {
          "line_address": "0x1000",
          "hit": true,
          "set": 0,
          "way": 2,
          "evicted_address": null
        },
        {
          "line_address": "0x1040",
          "hit": false,
          "set": 1,
          "way": 3,
          "evicted_address": "0x2040"
        }
      ]
    }
  ]
}
```

### Event

Каждый элемент `events` соответствует одному событию исходной trace.

| Поле | Тип | Описание |
|---|---|---|
| `sequence` | integer | исходный номер события в trace |
| `kind` | string | тип операции: `read` или `write` |
| `address` | string | адрес, переданный симулятору |
| `size_bytes` | integer | размер обращения в байтах |
| `lines` | array | результаты обращения к затронутым строкам кэша |

`sequence` сохраняется из исходной trace, чтобы можно было сопоставить результат симуляции с исходным событием.

`address` содержит адрес, фактически переданный в `ICacheSimulator::read()` или `ICacheSimulator::write()`. Для `memref_offset` это адрес после применения model base.

### Line result

Каждый элемент `lines` напрямую соответствует `LineAccessResult` из API симулятора.

| API field | JSON field | Описание |
|---|---|---|
| `line_address` | `line_address` | начальный адрес строки кэша |
| `hit` | `hit` | результат обращения |
| `set` | `set` | индекс набора |
| `way` | `way` | way, содержащий строку после обращения |
| `evicted_address` | `evicted_address` | адрес вытесненной строки или `null` |

`lines` формируется из `AccessResult::lines`.

Адреса (`address`, `line_address`, `evicted_address`) сериализуются строками в шестнадцатеричном формате, чтобы избежать потери точности при работе с 64-битными значениями в JavaScript.

## 3. Правила формирования результатов

Симулятор и программа запуска должны:

- читать исходную trace без её изменения;
- передавать обращения в `ICacheSimulator::read()` и `ICacheSimulator::write()`;
- связывать каждый `AccessResult` с исходным событием trace;
- сохранять результаты каждой затронутой cache line;
- накапливать статистику во время симуляции;
- записывать отдельные результаты для каждого варианта и конфигурации кэша.

Счётчики статистики могут накапливаться непосредственно во время обработки событий. Хранить все события в памяти для расчёта `statistics.json` не требуется.

## 4. Совместимость

До появления отдельной библиотеки для проверки схем потребители должны:

- проверять точное значение `schema`;
- игнорировать неизвестные дополнительные поля;
- трактовать `null` как недоступное значение, а не как ноль;
- использовать целочисленные счётчики как источник истины для расчёта rates;
- не считать `evictions` эквивалентом `misses`;
- сохранять 64-битные адреса без потери точности.

Изменения, нарушающие смысл существующих полей, требуют новой версии схемы.

## 5. Версии схем

| Schema | Назначение |
|---|---|
| `cachescope.trace.v1` | исходная trace от compiler runtime |
| `cachescope.simulation-statistics.v1` | итоговая статистика симуляции |
| `cachescope.simulation-events.v1` | результаты отдельных обращений к кэшу |

Compiler runtime отвечает за создание trace. Cache simulator отвечает за результаты симуляции. UI использует эти данные независимо.
