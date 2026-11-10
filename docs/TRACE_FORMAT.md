# Trace format: `cachescope.trace.v1`

`cachescope.trace.v1` — текущий wire contract между compiler trace producer,
cache emulator и UI. Внешних пользователей у ранней версии не было, поэтому
формат v1 был исправлен in-place: logical indices удалены, static access table
отделена от dynamic events, добавлены status и producer metadata.

Машиночитаемая схема находится в
[`schemas/cachescope.trace.v1.schema.json`](../schemas/cachescope.trace.v1.schema.json).
Optional stage-aware mapping для UI описан отдельно в
[`SOURCE_MAPPING.md`](SOURCE_MAPPING.md).

Схема намеренно не объявляет публичный `$id`: канонического опубликованного
URL у проекта пока нет. Поле `$schema` указывает только на версию стандарта
JSON Schema. Валидатор запускается так:

```bash
python3 scripts/validate_trace.py path/to/trace.json
```

## Полный объект

```json
{
  "schema": "cachescope.trace.v1",
  "producer": {
    "name": "CacheScope",
    "version": "0.1.0",
    "llvm_version": "24.0.0git",
    "llvm_revision": "40e5c847efaca209a9823e950fc5f26dc60cad4e",
    "pipeline_fingerprint": "torch-mlir-matmul-baseline-v1"
  },
  "variant": "baseline",
  "valid": true,
  "complete": true,
  "total_events": 1,
  "recorded_events": 1,
  "truncated": false,
  "errors": [],
  "accesses": [],
  "events": []
}
```

## Header и инварианты

| Поле | Тип | Смысл |
|---|---|---|
| `schema` | string | точное значение `cachescope.trace.v1` |
| `producer` | object | версия CacheScope, LLVM revision и fingerprint pipeline |
| `variant` | string | имя compiler variant |
| `valid` | boolean | runtime не обнаружил нарушения ABI/contract |
| `complete` | boolean | trace одновременно valid и не truncated |
| `total_events` | integer | все вызовы dynamic trace ABI |
| `recorded_events` | integer | сохранённый префикс events |
| `truncated` | boolean | часть dynamic events отброшена по limit |
| `errors` | string[] | обнаруженные runtime contract errors |
| `source_map` | object | optional `cachescope.source-map.v1` metadata |

Обязательные соотношения:

```text
valid == errors.empty
truncated == total_events > recorded_events
complete == valid && !truncated
recorded_events == events.length
```

`pipeline_fingerprint` задаётся через
`CACHESCOPE_PIPELINE_FINGERPRINT`. Он идентифицирует orchestration pipeline, а
не содержимое входных tensors.

## Static access table

Compiler вставляет регистрации всех известных access sites в пролог target
function. Поэтому `accesses` не строится из записанного dynamic prefix: в
таблице остаются неисполненные sites и sites, события которых оказались за
`CACHESCOPE_TRACE_LIMIT`.

```json
{
  "access_id": 0,
  "memref_id": 0,
  "kind": "read",
  "address_kind": "memref_offset",
  "source_location": "loc(\"kernel.mlir\":12:7)"
}
```

`source_location` остаётся диагностическим текстом и не является
cross-variant identity. Если producer публикует `source_map`, structured
`origin_ids` и `stage_locations` являются источником истины для UI.
Compiler передаёт эти metadata в runtime типизированными ABI calls. Runtime
проверяет ссылки между stages, origins и accesses и сериализует расширение
только целиком; при нарушении ABI трасса остаётся валидным JSON с
`valid = false`, а неполный `source_map` не публикуется.

Для LLVM fallback используются `memref_id = -1` и
`address_kind = "process_address"`.

## Semantic event

```json
{
  "sequence": 0,
  "access_id": 0,
  "memref_id": 0,
  "byte_offset": 16,
  "size_bytes": 4,
  "kind": "read",
  "address_kind": "memref_offset"
}
```

`byte_offset` считается от начала root memref. Simulator сам назначает
детерминированный model base каждому `memref_id`.

Logical MLIR indices намеренно не являются частью core event: cache replay
использует byte address и access size, а фиксированное число индексов мешало бы
поддержке arbitrary-rank memrefs. При необходимости UI metadata может быть
добавлена в будущем как optional extension.

## LLVM fallback event

```json
{
  "sequence": 0,
  "access_id": 4294967296,
  "memref_id": -1,
  "byte_offset": 4660,
  "size_bytes": 8,
  "kind": "read_write",
  "address_kind": "process_address"
}
```

При `process_address` поле `byte_offset` содержит абсолютный virtual address
процесса. Такие адреса нельзя сопоставлять между разными запусками.

## Access kinds

- `read`;
- `write`;
- `read_write` для atomic RMW/cmpxchg fallback.

Значение `invalid` может появиться только в diagnostic trace с
`valid = false`; consumer обязан отклонить такой trace до replay.
Diagnostic trace также может сохранить отрицательный `access_id`, полученный
через нарушенный ABI. В корректном trace `access_id` всегда неотрицателен.

## Runtime configuration

| Environment variable | Default | Назначение |
|---|---|---|
| `CACHESCOPE_TRACE_FILE` | `trace.json` | output path |
| `CACHESCOPE_VARIANT` | `unknown` | variant name |
| `CACHESCOPE_PIPELINE_FINGERPRINT` | `unspecified` | stable ID compiler pipeline |
| `CACHESCOPE_TRACE_LIMIT` | `1000000` | maximum recorded events |

Runtime сначала пишет `<trace>.tmp`, а затем публикует готовый JSON rename-ом.
Trace limit принимает только непустое десятичное unsigned-значение; неверная
настройка сохраняется как contract error и даёт `valid = false`.

## Compatibility policy

Потребители обязаны:

- требовать точное известное значение `schema`;
- перед replay проверять `valid`, `complete` и `errors`;
- игнорировать неизвестные дополнительные поля;
- не интерпретировать `byte_offset` без `address_kind`;
- проверять event против записи с тем же `access_id`;
- при наличии `source_map` проверять его schema и все ссылки на stages/origins;
- не сопоставлять fallback addresses между процессами.

Golden fixtures находятся в [`tests/fixtures`](../tests/fixtures). Любое
изменение обязательного поля или его семантики требует обновления JSON Schema,
валидатора, fixtures, runtime suite и этого документа.
