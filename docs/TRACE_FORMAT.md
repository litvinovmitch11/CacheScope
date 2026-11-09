# Trace format: `cachescope.trace.v1`

Runtime записывает один JSON object.

```json
{
  "schema": "cachescope.trace.v1",
  "variant": "baseline",
  "producer": "instrumented-mlir-execution",
  "total_events": 1,
  "recorded_events": 1,
  "truncated": false,
  "accesses": [],
  "events": []
}
```

## Header

| Поле | Тип | Смысл |
|---|---|---|
| `schema` | string | версия wire format |
| `variant` | string | значение `CACHESCOPE_VARIANT` |
| `producer` | string | источник trace |
| `total_events` | integer | все runtime calls |
| `recorded_events` | integer | реально сохранённые events |
| `truncated` | boolean | `total_events > recorded_events` |

## Static access table

Каждая запись `accesses` содержит:

```json
{
  "access_id": 0,
  "memref_id": 0,
  "kind": "read"
}
```

Таблица строится по записанным events. При truncation access site, который не
попал в сохранённый префикс, отсутствует.

## Semantic event

```json
{
  "sequence": 0,
  "access_id": 0,
  "memref_id": 0,
  "byte_offset": 16,
  "size_bytes": 4,
  "kind": "read",
  "indices": [0, 4],
  "address_kind": "memref_offset"
}
```

`byte_offset` считается от логического начала root memref. Simulator должен
сам назначить детерминированный model base каждому `memref_id`.

## LLVM fallback event

```json
{
  "sequence": 0,
  "access_id": 4294967296,
  "memref_id": -1,
  "byte_offset": 4660,
  "size_bytes": 8,
  "kind": "read_write",
  "indices": [-1, -1],
  "address_kind": "process_address"
}
```

Для совместимости поле физического значения пока называется `byte_offset`, но
при `process_address` содержит абсолютный virtual address процесса. Потребитель
обязан сначала проверять `address_kind`.

## Access kinds

- `read`;
- `write`;
- `read_write` для atomic RMW/cmpxchg fallback.

## Runtime configuration

| Environment variable | Default | Назначение |
|---|---|---|
| `CACHESCOPE_TRACE_FILE` | `trace.json` | output path |
| `CACHESCOPE_VARIANT` | `unknown` | variant name |
| `CACHESCOPE_TRACE_LIMIT` | `1000000` | maximum recorded events |

## Compatibility policy

До появления отдельной schema library потребители должны:

- требовать точное значение `schema`;
- игнорировать неизвестные дополнительные поля;
- не интерпретировать `byte_offset` без `address_kind`;
- учитывать `truncated` до любого сравнения вариантов;
- не сопоставлять fallback addresses между процессами.
