# Локальные эксперименты из UI

UI и API обслуживаются одним процессом на `http://localhost:9055`.
Запуск: `docker compose up --build`. Пользовательский пример находится в
[examples/ui-upload](../examples/ui-upload/README.md).

Это отдельный сценарий от [пошагового API runner](RUNNER_API.md).
Существующие sessions, интерфейсы кэшей и файловые trace/report схемы сохраняются.

| Метод | Путь | Назначение |
| --- | --- | --- |
| POST | `/api/v1/experiments` | multipart `mlir` и `inputs`, по одному файлу; 202 + Location |
| GET | `/api/v1/experiments` | `{experiments: []}` или один текущий descriptor |
| GET | `/api/v1/experiments/{id}` | текущий descriptor |
| POST | `/api/v1/experiments/{id}/run` | JSON параметров запуска; 202 |
| POST | `/api/v1/experiments/{id}/cancel` | отменить активную работу; 202, повторная отмена безопасна |
| GET | `/api/v1/experiments/{id}/report` | `{statistics, trace}` только после полного успеха |

Descriptor содержит `experiment_id`, `status`, `stage`, `metadata`, `spec`,
`error`. Статусы: preparing → ready → running → completed; ошибки дают failed;
отмена проходит через cancelling → cancelled. Этапы — inspection, compilation,
execution, simulation. Для ready/completed stage равен null.

Metadata содержит `functions` (name, supported, reason, arguments) и
`arrays` (key, shape, dtype). Аргументы имеют index, type и shape; привязки
задаются по порядку аргументов, без угадывания исходных SSA-имён.

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

Нужен Python с NumPy. Обычная standalone-сборка runner не требует LLVM;
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
