# Локальные эксперименты из UI

UI и API обслуживаются одним процессом на `http://localhost:9055`.
Запуск: `docker compose up --build`. Пользовательский пример находится в
[examples/ui-upload](../examples/ui-upload/README.md).

Это отдельный сценарий от [пошагового API runner](RUNNER_API.md).
Существующие sessions, интерфейсы кэшей и файловые trace/report схемы сохраняются.

| Метод | Путь | Назначение |
| --- | --- | --- |
| GET | `/api/v1/experiment-templates` | catalog встроенных experiment templates и их доступность |
| POST | `/api/v1/experiments` | multipart `mlir` + `inputs`, JSON template либо JSON Torch project; 202 + Location |
| GET | `/api/v1/experiments` | `{experiments: []}` или один текущий descriptor |
| GET | `/api/v1/experiments/{id}` | текущий descriptor |
| POST | `/api/v1/experiments/{id}/run` | JSON параметров запуска; 202 |
| POST | `/api/v1/experiments/{id}/cancel` | отменить активную работу; 202, повторная отмена безопасна |
| GET | `/api/v1/experiments/{id}/report` | `{statistics, trace}` только после полного успеха |
| GET | `/api/v1/experiments/{id}/results` | compact comparison entries Torch template/project |
| GET | `/api/v1/experiments/{id}/artifacts` | ссылки на опубликованные artifacts |
| GET | `/api/v1/experiments/{id}/artifacts/report` | HTML встроенного template; для Torch project redirect на UI report screen |
| GET | `/api/v1/experiments/{id}/artifacts/summary` | machine-readable summary |
| GET | `/api/v1/experiments/{id}/artifacts/stepper?variant={name}&profile={name}` | manifest trace/stages выбранного schedule и выбранной cache config |

Descriptor содержит `experiment_id`, `template_id`, `status`, `stage`,
`progress`, `metadata`, `spec`, `artifacts`, `error`. Статусы: preparing →
ready → running → completed; ошибки дают failed;
отмена проходит через cancelling → cancelled. Этапы — inspection, compilation,
execution, simulation. Для ready/completed stage равен null.

Metadata содержит `functions` (name, supported, reason, arguments) и
`arrays` (key, shape, dtype). Аргументы имеют index, type и shape; привязки
задаются по порядку аргументов, без угадывания исходных SSA-имён.
`inputs.npz` является обычным архивом `numpy.savez`: каждый именованный массив
становится отдельным `arrays[].key`. Для binding допускается только `float32`
массив с формой, точно равной статической форме соответствующего MLIR memref;
reshape, broadcasting и неявное преобразование dtype не выполняются.

Встроенный Torch experiment запускается тем же POST с JSON:

```json
{
  "template_id": "depthwise-temporal-blocking",
  "parameters": {"tile_sizes": [6, 10, 14], "stepper_events": 2048},
  "timeout_seconds": 1800
}
```

Он сразу переходит в running. `progress` содержит stage, message и, для
variant/cache loops, current, total и variant. После completed `results`
возвращает данные всех вариантов и двух cache profiles, а artifact manifest
позволяет UI открыть report и создать Stepper session без filesystem paths.

## Редактируемый локальный Torch project

Experiment Studio отправляет весь доверенный проект в одном JSON-запросе:

```json
{
  "schema": "cachescope.torch-project.v1",
  "name": "My locality experiment",
  "files": {
    "model.py": "import torch\ndef create_model(): ...\n",
    "inputs.py": "import torch\ndef create_inputs(): ...\n"
  },
  "variants": [
    {"name": "baseline", "transform": "module attributes {transform.with_named_sequence} {...}"},
    {"name": "tiled", "transform": "module attributes {transform.with_named_sequence} {...}"}
  ],
  "cache_profiles": {
    "l1-small": ["L1:16384:64:4:lru"],
    "intel-dx2-l1-wt": {
      "levels": [{
        "name": "L1", "size_bytes": 8192, "line_size_bytes": 16,
        "associativity": 4, "replacement_policy": "PLRU4",
        "write_policy": "write_through",
        "allocation_policy": "no_write_allocate"
      }],
      "hierarchy": {
        "inclusion_policies": [], "writeback_clean": false,
        "non_inclusive_fill": "all_levels"
      }
    }
  },
  "settings": {
    "stepper_events": 2048,
    "trace_limit": 2000000,
    "validation": {"rtol": 0.00001, "atol": 0.00001}
  },
  "timeout_seconds": 1800
}
```

`model.py` экспортирует `create_model() -> torch.nn.Module`, `inputs.py` —
`create_inputs() -> tuple[torch.Tensor, ...]`. Каждый schedule — независимый
`@__transform_main`; ошибка одного варианта публикуется в `variants`, остальные
продолжают pipeline. Код исполняется локально без sandbox как код доверенного
пользователя. Контракт: [JSON Schema](../schemas/cachescope.torch-project.v1.schema.json).
В job descriptor исходники из `files` и `variants[].transform` не повторяются:
они остаются в локальном draft UI, а polling передаёт только manifest metadata и
progress.

Текущая поддерживаемая граница: CPU, float32, положительные static shapes,
один Tensor на выходе и функция `forward`. Worker один раз вычисляет
`model(*inputs)` в eager Torch, экспортирует Linalg-on-tensors, компилирует
каждый schedule и вызывает
`torch.testing.assert_close(compiled, eager, rtol=..., atol=...)`,
снимает semantic trace и применяет одинаковые cache profiles ко всем успешным
вариантам. Compact profile использует runner-строки; structured profile
`{levels, hierarchy}` сохраняет write/allocation и межуровневые policies.
Для каждого успешного schedule с ненулевым `stepper_events` worker
публикует отдельный `stepper/<variant>/` bundle. UI report требует явного
выбора варианта и cache profile и передаёт их в Stepper manifest endpoint.
Manifest возвращает нормализованные `levels`, `hierarchy`, а также фактически
выбранные `variant` и `profile`. Stepper создаёт interactive session с теми же
inclusion/writeback/fill policies, которые использовались при batch replay, и
показывает этот контекст над сессией.

Пример `run`:

```json
{
  "function": "copy_kernel",
  "bindings": [{"source":"npz","key":"source"},{"source":"zeros"}],
  "levels": [{"name":"L1","size_bytes":128,"line_size_bytes":64,"associativity":2,"replacement_policy":"LRU"}],
  "timeout_seconds": null,
  "trace_limit": null
}
```

`levels` используют геометрию и policies из runner-контракта: одинаковая строка
на всех уровнях, LRU/FIFO/Random/PLRU4. Defaults — exclusive/write-back/write-allocate.
Каждый уровень допускает `write_policy` (`write_back`/`write_through`) и
`allocation_policy` (`write_allocate`/`no_write_allocate`). Необязательный
`hierarchy` содержит `inclusion_policies` для N−1 пар, `writeback_clean` и
`non_inclusive_fill`; [точная семантика](CACHE_MODELS.md#политики-записи-и-иерархии).
Необязательный `cache_profile` — информационная строка до 128 байтов,
не влияет на симуляцию. Если профиль удалён из каталога UI, клиент восстанавливает
`levels` и `hierarchy` как ручную конфигурацию без названия процессора.
Random допускает `random_seed` в виде канонической uint64 decimal string,
по умолчанию `"0"`. Seed у других policies отклоняется.

`timeout_seconds` — положительное конечное число или null. `trace_limit` —
положительная uint64 decimal string или null. Null означает без ограничений;
жёстких серверных максимумов для времени и событий нет. Timeout охватывает
компиляцию, исполнение и симуляцию, начиная с принятия run; подготовку файлов
можно отменить отдельно. Превышение лимита трассы завершает запуск ошибкой.

400 означает неверные параметры и оставляет ready-эксперимент доступным для
исправления. 409 — активный эксперимент, повторный run или ещё неготовый
report. 404 — неизвестный/заменённый id. Ошибки worker публикуются в descriptor
с code/message и доступной диагностикой; частичный report не выдаётся.

Новый upload заменяет предыдущий эксперимент, если тот не активен.
Браузерные POST-запросы разрешены только с того же origin или с явно
настроенного `--allow-origin`; запросы curl без Origin поддерживаются.
Подготовленные файлы неизменны; один id запускается не более одного раза.
Обновление страницы не отменяет работу: GET списка позволяет восстановить её.
При остановке backend worker завершается, временные файлы удаляются.

Для нативного окружения с собранными LLVM, compiler/runtime, runner и UI:

```bash
CACHESCOPE_BUILD_DIR=/absolute/path/build/app LLVM_ROOT=/usr/lib/llvm-22 \
  build/app/runner/cachescope_runner_server --app-dir "$PWD"
```

Для встроенного template нужен `venv/bin/python` с
`requirements-torch-demo.lock` либо `CACHESCOPE_PYTHON=/path/to/python`.
Catalog возвращает `available: false`, если pinned Python environment или
compiler artifacts отсутствуют. Для MLIR upload нужен Python с NumPy. Обычная standalone-сборка runner не требует LLVM;
без `--app-dir` API экспериментов возвращает 503, а sessions работают как раньше.
Для разработки UI Vite проксирует `/api` на локальный порт 9055.

Проверки полного приложения внутри поднятого контейнера:

```bash
docker compose exec -T cachescope python3 tests/experiment_suite.py --url http://127.0.0.1:9055 --repo .
```

Браузерная проверка: `cd ui && npm ci && npx playwright install chromium && npm run test:e2e`.
Она использует настоящий backend на 9055; другой URL задаётся `CACHESCOPE_E2E_URL`.

Время и число событий не ограничиваются по умолчанию, но runtime хранит события
в памяти: доступные память и диск контейнера остаются физическими пределами.
