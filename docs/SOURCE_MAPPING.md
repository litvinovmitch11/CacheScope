# Stage-aware source mapping: `cachescope.source-map.v1`

`cachescope.source-map.v1` — optional embedded extension
`cachescope.trace.v1`. Она связывает:

```text
строку выбранного MLIR dump
  → static access_id
  → dynamic event.sequence
  → результат cache simulation
```

Контракт описан и проверяется validator-ом. Канонический producer назначает
origins, переносит tagged locations через transformations, публикует пять
stage dumps и встраивает mapping в итоговый trace.

## Почему одного source location недостаточно

Одна high-level операция может породить несколько конкретных обращений. Для
`linalg.matmul` после lowering это, например, отдельные `memref.load` для A, B
и C и `memref.store` для C. Поэтому UI должен позволять выбрать стадию IR:

```text
input
after-transform
after-linalg-to-loops
after-instrumentation
after-llvm-lowering
```

На `input` несколько access sites могут указывать на одну строку
`linalg.matmul`. На `after-linalg-to-loops` те же sites указывают на разные
строки `memref.load/store`.

Legacy-поле `accesses[].source_location` остаётся строковой диагностикой и не
является источником stage-aware mapping. UI использует только структурированные
поля этого контракта.

## Представление в trace

```json
{
  "source_map": {
    "schema": "cachescope.source-map.v1",
    "payload_fingerprint": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "stages": [
      {
        "stage_id": "input",
        "path": "input.mlir"
      },
      {
        "stage_id": "after-transform",
        "path": "tile4/01-transformed.mlir"
      },
      {
        "stage_id": "after-linalg-to-loops",
        "path": "tile4/02-loops.mlir"
      }
    ],
    "origins": [
      {
        "origin_id": 0,
        "stage_id": "input",
        "line": 8,
        "column": 5,
        "operation": "linalg.matmul"
      }
    ]
  },
  "accesses": [
    {
      "access_id": 0,
      "memref_id": 0,
      "kind": "read",
      "address_kind": "memref_offset",
      "source_location": "loc(\"tile4/02-loops.mlir\":16:22)",
      "origin_ids": [0],
      "stage_locations": [
        {
          "stage_id": "after-transform",
          "line": 14,
          "column": 9,
          "operation": "linalg.matmul"
        },
        {
          "stage_id": "after-linalg-to-loops",
          "line": 16,
          "column": 22,
          "operation": "memref.load"
        }
      ]
    }
  ]
}
```

Dynamic events не дублируют mapping. Они уже содержат `access_id` и получают
mapping через static access table независимо от event count и truncation.

## Source map header

| Поле | Тип | Смысл |
|---|---|---|
| `schema` | string | точное значение `cachescope.source-map.v1` |
| `payload_fingerprint` | string | lowercase SHA-256 точных bytes файла input stage с префиксом `sha256:` |
| `stages` | object[] | упорядоченные стадии pipeline и пути к их dumps |
| `origins` | object[] | логические high-level operations входного payload |

`payload_fingerprint` задаёт область стабильности `origin_id`. Сопоставлять
origins разных traces разрешено только при одинаковом fingerprint.

## Stages

Каждый stage содержит:

| Поле | Тип | Смысл |
|---|---|---|
| `stage_id` | string | стабильный ID вида `^[a-z][a-z0-9-]*$` |
| `path` | string | путь к MLIR dump относительно каталога, содержащего trace JSON |

Первый stage всегда имеет `stage_id = "input"`. Порядок массива является
порядком pipeline. `stage_id` и `path` уникальны.

Путь использует `/`, не является абсолютным и не содержит пустые, `.` или
`..` components. Trace bundle остаётся переносимым между машинами.

CLI validator разрешает эти пути относительно каталога trace, проверяет
наличие каждого dump и сверяет `payload_fingerprint` с точными байтами файла
`input`. Библиотечный вызов без artifact root проверяет только структуру и
cross-field invariants.

Canonical stage IDs первой producer implementation:

| Stage ID | Содержимое |
|---|---|
| `input` | исходный high-level MLIR payload |
| `after-transform` | payload после выбранной Transform Dialect strategy |
| `after-linalg-to-loops` | конкретные loops и `memref.load/store` |
| `after-instrumentation` | IR после назначения `access_id` и trace calls |
| `after-llvm-lowering` | LLVM dialect до native execution |

Будущие producers могут добавлять stages. Consumer не должен полагаться на
фиксированное число стадий.

## Producer pipeline

Канонический driver использует штатный MLIR `snapshot-op-locations` и два
CacheScope passes:

- `cachescope-assign-origins` детерминированно нумерует high-level memory
  operations до Transform Dialect;
- `cachescope-tag-stage` сохраняет полное имя operation рядом с location
  snapshot выбранной стадии.

Transformations переносят вложенные locations на созданные operations.
`cachescope-instrument-memory` добавляет `access_id`, а финальный
`cachescope-instrument-llvm-fallback` собирает origin lineage и stage
locations, после чего регистрирует готовое static metadata в runtime.

Если driver передал `payload-fingerprint`, но хотя бы один semantic access
потерял origin или stage location, compilation завершается ошибкой. Producer
не выпускает частично размеченный `source_map`.

## Origins

Origin описывает high-level operation в `input`:

| Поле | Тип | Смысл |
|---|---|---|
| `origin_id` | integer | уникальный неотрицательный ID в пределах fingerprint |
| `stage_id` | string | в v1 всегда `input` |
| `line` | integer | 1-based строка operation в dump |
| `column` | integer | 0-based колонка начала operation в dump |
| `operation` | string | MLIR operation name, например `linalg.matmul` |

`origin_id` назначается детерминированно до transformations. Один origin может
породить несколько access sites при tiling, cloning и lowering. Один access
может иметь несколько `origin_ids`, чтобы контракт не запрещал fusion.

## Access mapping

Каждый элемент `accesses` описывает один static memory access site:

| Поле | Тип | Смысл |
|---|---|---|
| `access_id` | integer | уникальный неотрицательный ID static access site; по нему dynamic `events` ссылаются на эту запись |
| `memref_id` | integer | ID semantic memref; для LLVM fallback используется `-1` |
| `kind` | string | `read`, `write` или `read_write` |
| `address_kind` | string | `memref_offset` для semantic access либо `process_address` для fallback |
| `source_location` | string | legacy diagnostic location; не используется как structured UI mapping |
| `origin_ids` | integer[] | optional ссылки на high-level operations из `source_map.origins` |
| `stage_locations` | object[] | optional позиции этого access site в сохранённых stage dumps |

Элемент `stage_locations` содержит:

| Поле | Тип | Смысл |
|---|---|---|
| `stage_id` | string | ссылка на `source_map.stages[].stage_id` |
| `line` | integer | 1-based строка operation в dump |
| `column` | integer | 0-based колонка начала operation в dump |
| `operation` | string | полное MLIR operation name, например `memref.load` |

При наличии `source_map` каждый semantic access с
`address_kind = "memref_offset"` обязан содержать:

- непустой уникальный `origin_ids`;
- непустой `stage_locations`.

`stage_locations` содержит 1-based line, 0-based column и operation name в
dump указанного stage. Такая база координат совпадает с
`AsmState::LocationMap` и `snapshot-op-locations`: колонка указывает на начало
печатного operation, включая SSA result list перед именем op. Несколько
locations одного stage допустимы для lineage, образованного слиянием
operations.

LLVM fallback с `address_kind = "process_address"` может не иметь high-level
origin. Для него mapping fields могут отсутствовать либо быть пустыми.

## UI navigation

Из dynamic event в IR:

1. взять `event.access_id`;
2. найти static access с тем же ID;
3. найти `stage_locations` выбранного UI stage;
4. открыть соответствующий `stages[].path` и подсветить locations.

Из строки IR в events:

1. определить `stage_id`, line и column выбранной operation;
2. найти static accesses с совпадающим `stage_location`;
3. выбрать events, `access_id` которых входит в найденное множество.

Для `input` UI сначала находит origins по line/column, затем static accesses,
содержащие соответствующий `origin_id`.

UI обязан показывать все совпадения, а не выбирать одно произвольно.

## Compatibility

- `source_map` optional, поэтому существующие `trace.v1` остаются валидными;
- наличие объекта означает поддержку `cachescope.source-map.v1`;
- неизвестное значение `source_map.schema` отклоняется;
- неизвестные дополнительные поля игнорируются;
- `origin_id` нельзя сравнивать между разными `payload_fingerprint`;
- mapping является static metadata и не зависит от `truncated`;
- line/column относятся к сохранённому dump, а не к произвольной повторной
  печати того же MLIR.

Golden fixture находится в
[`tests/fixtures/source-map-v1`](../tests/fixtures/source-map-v1).
