# Runner HTTP API v1: примеры

Примеры относятся к [OpenAPI](../../cachescope.runner.v1.openapi.json) и
[семантике API](../../../docs/RUNNER_API.md). Это контрактные примеры,
а не запись конкретного HTTP-запуска.

Каждый JSON содержит `exchanges`: name, schema_valid_request, request
(method, path, query, body) и response (status, headers, body).
Путь включает `/api/v1`. JSON-значения query перед сериализацией приводятся
к тексту. Тело и Content-Type отсутствуют у ответа DELETE 204.
`schema_valid_request=false` означает намеренное нарушение формы запроса;
динамические нарушения геометрии, revision и лимитов могут проходить
структурную проверку и отклоняются по правилам API.

- [replay.json](replay.json) — последовательный FIFO replay: подготовка,
  cold miss, L1 hit, dirty eviction, L2 promotion, dirty LLC writeback,
  multiline, EOF/no-op, страницы инструкций, reset и DELETE.
- [errors.json](errors.json) — независимые ситуации с конфликтами, лимитами,
  ошибками подготовки/исполнения и обнаруженным изменением трассы.
- [edge-cases.json](edge-cases.json) — независимые Random/reset и пустая трасса.
  Random имеет один way: жертва однозначна независимо от standard library;
  fixture иллюстрирует эффективный seed и одинаковые результаты после reset,
  не проверяет качество RNG.
- [wide-values.json](wide-values.json) — отдельная подготовленная сессия с
  ID/counters/revision и адресами больше 2^53, отрицательным byte_offset
  и optional source map. Это синтетические metadata без MLIR bundle.

Для основного replay L1: 128 B, 2 ways; L2: 64 B, 1 way; строка 64 B,
по одному set, FIFO. Один memref с base `0x10000000`; read access_id=10,
write access_id=20, необработанный read site=30. События (offset, bytes):
`W(0,4), R(64,4), R(0,4), R(128,4), R(0,4), R(192,4), R(256,4),
R(320,4), R(380,8)`. Последнее касается двух строк.

После 9 событий: L1 hits=2/misses=8 (20%), L2 hits=1/misses=7 (12.5%),
memory reads=7/writebacks=1. Dirty-жертва L1 не означает запись в память:
после события 4 линия `0x10000000` снова в L1 с dirty=true, а L2 содержит
чистую `0x10000040`. В итоговом L1 — `0x10000140` и `0x10000180`,
L2 — `0x10000100`. Пустой site=30 остаётся в таблице с нулями.

Проверка структуры OpenAPI внешним валидатором, без изменения зависимостей
проекта:

```bash
python3 -m venv /tmp/cachescope-api-validation
/tmp/cachescope-api-validation/bin/pip install openapi-spec-validator==0.9.0
/tmp/cachescope-api-validation/bin/openapi-spec-validator schemas/cachescope.runner.v1.openapi.json
python3 tests/schema_suite.py --repo .
python3 tests/documentation_suite.py --repo .
```

Для проверки тел consumer выбирает operation по method/path, response по
status и проверяет application/json schema соответствующего сообщения.
Отдельно проверяются параметры path/query, связи sequence/cursor/revision,
численные диапазоны custom formats и правила согласованности из документа.
Fixtures не требуют запуска LLVM, runner или HTTP-сервера.
