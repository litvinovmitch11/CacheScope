# Интерактивный runner: HTTP API v1

Это контракт локального backend для пошагового просмотра из UI.
Сборка и запуск сервера описаны в [runner README](../runner/README.md).
Машинное описание — [OpenAPI 3.1](../schemas/cachescope.runner.v1.openapi.json),
запросы и ответы — [fixtures](../schemas/fixtures/runner-api-v1/README.md).
OpenAPI задаёт форму сообщений; правила ниже дополняют её ограничениями
между полями, состояниями и запросами. Оба документа нормативны.

## Транспорт и маршруты

Launcher передаёт UI фактический base URL локального backend, например
`http://127.0.0.1:9055/api/v1`. Порт не закреплён контрактом. Тела запросов
и ответов — `application/json`; исключение — DELETE с пустым ответом 204.
Способ обслуживания UI, CORS и выбор Boost/другого HTTP-сервера остаются
за реализацией. В v1 нет WebSocket/SSE и фонового исполнения.

| Метод и путь относительно `/api/v1` | Назначение | Успех |
|---|---|---|
| `POST /sessions` | Зарезервировать сессию и начать подготовку | 202 + `Session`, `Location` |
| `GET /sessions` | Найти текущую сессию после перезагрузки UI | 200 + `SessionList` |
| `GET /sessions/{id}` | Прочитать статус, позицию, revision, конфигурацию и лимиты | 200 + `Session` |
| `DELETE /sessions/{id}?expected_revision=…` | Освободить ресурс | 204 |
| `GET /sessions/{id}/metadata` | Получить неизменные metadata трассы | 200 + `Metadata` |
| `POST /sessions/{id}/step` | Обработать `count` событий | 200 + `StepResponse` |
| `POST /sessions/{id}/reset` | Вернуться в исходное холодное состояние | 200 + `Progress` |
| `GET /sessions/{id}/state?expected_revision=…&level=…&set_offset=…&set_limit=…` | Получить страницу физических слотов одного уровня и общую статистику | 200 + `StateResponse` |
| `GET /sessions/{id}/instructions?expected_revision=…&offset=…&limit=…` | Получить страницу статистики инструкций | 200 + `InstructionsResponse` |

Одна сессия занимает backend при любом статусе, включая `finished` и `failed`.
Повторный POST до явного DELETE возвращает `409 session_exists`.
GET списка возвращает ноль или один элемент. Сессия существует до DELETE
или перезапуска процесса; сохранение и восстановление между процессами
не предусмотрены. `session_id` непрозрачен и не переиспользуется для другой
сессии в пределах экземпляра backend. После перезапуска UI заново обнаруживает
сессию; прежние ссылки и revision использовать нельзя.

## Создание и подготовка

Тело создания содержит абсолютный `trace_path` в файловой системе backend
и упорядоченный массив `levels`, начиная с L1:

```json
{
  "trace_path": "/workspace/traces/fifo.json",
  "levels": [
    {"name": "L1", "size_bytes": 128, "line_size_bytes": 64,
     "associativity": 2, "replacement_policy": "FIFO"},
    {"name": "L2", "size_bytes": 64, "line_size_bytes": 64,
     "associativity": 1, "replacement_policy": "Random", "random_seed": "42"}
  ]
}
```

Геометрия соответствует существующему SetAssociativeCacheLevel:
положительные размер и associativity, целое число полных sets,
`set_count = size_bytes / (line_size_bytes * associativity)`.
Размер строки и число sets — степени двойки. Размер строки одинаков на всех
уровнях, имена уровней уникальны. `set_count` вычисляет backend и возвращает
в `Session.levels`, клиент его не задаёт.

Политики замещения: `LRU`, `FIFO`, `Random`, `PLRU4`. `PLRU4` требует associativity=4. Только `Random` принимает
`random_seed`; отсутствие означает `"0"`. Каждому set создаётся отдельная
policy с этим seed. Эффективный seed возвращается строкой, для других policy —
`null`. Reset восстанавливает RNG и порядок замещения. Повторяемость означает
одинаковые trace/config/seed в одной реализации backend; контракт не задаёт
битовую совместимость Random между разными реализациями стандартной библиотеки.
Defaults: `exclusive`, `write_back`, `write_allocate`.
`levels[]` допускает `write_policy` и `allocation_policy`; необязательный
`hierarchy` — `inclusion_policies` для каждой пары, `writeback_clean` и
`non_inclusive_fill`. Ответ содержит эффективные политики каждого уровня и
описание связей; неодинаковые политики сводятся в `mixed`.
[Семантика и пример конфигурации](CACHE_MODELS.md#политики-записи-и-иерархии).

Ошибка формы запроса или конфигурации — 400 до резервирования ресурса.
Корректный запрос возвращает 202, `Location: /api/v1/sessions/{id}` и
`status=preparing`, `cursor="0"`, `revision="0"`, `total_events=null`.
UI опрашивает GET сессии; прогресс чтения файла не нормирован.

Подготовка полностью проверяет трассу и вычисляет размещение буферов до
первого step. Поддерживается тот же subset, что у
[текущего runner](../runner/README.md): `cachescope.trace.v1`, `valid=true`,
`complete=true`, `truncated=false`, `errors=[]`, `memref_offset` и `read/write`.
Неподдерживаемые `process_address`, `read_write`, неполные и invalid traces
отклоняются. Проверяются типы, producer, последовательность событий от 0
без пропусков, соответствие static access table и переполнения арифметики.
Привязки source map следуют [SOURCE_MAPPING.md](SOURCE_MAPPING.md).
Наличие source map не обязательно; подготовка не требует читать тексты stages.

Буферы размещаются по возрастанию численного `memref_id`, начиная с
`0x10000000`; каждая база выровнена на 64 байта. Envelope включает смещение 0:
`min_offset <= 0`, `end_offset >= 0` — исключённый конец. Отрицательные
byte offsets сохраняются. Непересекающиеся диапазоны учитывают отрицательную
часть: следующая база — выровненное вверх значение предыдущего конца плюс
модуль отрицательного min_offset следующего буфера. Доступы вычисляются как
`base_address + byte_offset` с проверкой диапазонов uint64/int64. Размещение
не зависит от числа уже выполненных событий и сохраняется при reset.

После подготовки публикуется revision 1 и `ready`, либо сразу `finished`
для пустой трассы. При ошибке публикуется `failed` с диагностикой и
`recovery=recreate`; GET сессии при этом возвращает 200. Trace/config/layout
неизменны в течение жизни ресурса. Для другой конфигурации нужен DELETE/create.
Файл должен оставаться неизменным; обязательного копирования файла контракт
не требует. При обнаруженном изменении — `trace_changed`, `failed`, только
DELETE/create, включая запрет reset.

## Шаг, результат и reset

`cursor=N` означает, что полностью выполнены первые N событий, с sequence
от 0 до N−1. Холодная сессия имеет cursor 0, все слоты invalid, нулевые
счётчики и `last_event=null`.

Step принимает `{"expected_revision":"1","count":10}`. По умолчанию count=1;
count положителен и не больше `limits.max_step_count`. Команда синхронна.
Одно событие неделимо для наблюдателя: завершены все затронутые строки,
probes, продвижения, каскадные вытеснения и dirty writebacks. Промежуточные
состояния внутри события или batch не видны через API.

Успешный ответ содержит все `events` именно этого batch, `executed_count`,
финальные cursor/revision/status, накопительную `summary` и `last_event`.
`events.length == executed_count`, sequence последовательны от прежнего cursor,
`last_event` совпадает с последним завершённым событием всей сессии.
Ответ не содержит снимок всех caches после каждого события.

Если count больше оставшегося числа событий, выполняется остаток и
возвращается `finished`. На EOF step остаётся успешным: executed_count=0,
events=[], cursor/revision/summary/last_event не меняются. Нет фоновой команды
continue/pause, breakpoints, seek, обратного шага или серверного архива состояний.
UI может строить временной ряд по полученным событиям и summary.

Reset принимает только expected_revision и возвращает `Progress`:
cursor 0, холодные caches, нулевую статистику, last_event=null. Session ID,
trace, конфигурация, layout и seed остаются прежними. Для непустой трассы
статус становится ready, для пустой — finished. Revision увеличивается
даже при reset уже холодной сессии. Reset разрешён также после ошибки
исполнения с `recovery=reset`, но не во время подготовки.

## Revision и согласованное чтение

Revision — монотонный счётчик опубликованных изменений, независимый от cursor.
Успешная подготовка, непустой step, reset и переход в failed увеличивают
его; один batch увеличивает его один раз. Он не обнуляется при reset.
Чтение, отклонённый запрос и пустой step на EOF его не меняют.

Step, reset и DELETE требуют expected_revision. State и instructions тоже
требуют expected_revision: сервер не хранит старые снимки, несовпадение даёт
`409 revision_mismatch`. Metadata неизменны и revision не требуют.

Команды выполняются последовательно. Пока step/reset исполняется, другие
мутации, state и instructions получают `409 session_busy`. GET сессии/списка
доступен и показывает `busy=true` с последними опубликованными cursor/revision;
metadata доступны после успешной подготовки, в том числе при busy.
При reset из recoverable failed GET сохраняет status=failed и прежние
cursor/revision/error, но показывает busy=true до завершения команды.
DELETE допустим во время preparing, отменяет/отбрасывает подготовку, но при
выполнении синхронной команды получает session_busy. Гонка с завершением
подготовки разрешается проверкой revision, как и остальные гонки.

После потери ответа UI читает GET сессии и state, а не повторяет команду с
новой revision автоматически. Повтор со старой revision не может выполнить
ещё один шаг. Идентификаторы команд и хранилище ответов не нужны. Если ответ
потерян при EOF, повтор остаётся безопасным no-op. После неизвестного результата
create UI использует список сессий; после DELETE — список или GET с 404.

При загрузке нескольких страниц UI использует одну revision. После reset
или step любая старая страница становится устаревшей. При конфликте нужно
отбросить незавершённую сборку снимка, получить актуальную revision и перечитать
страницы; смешивать страницы разных revisions нельзя.

## Физические слоты и статистика

State выбирает ровно один `level` (от 1) и диапазон sets. Sets и ways
нумеруются от 0. Ответ содержит все ways каждого возвращённого set, в порядке
индексов, включая пустые: `valid=false`, `line_address=null`, `dirty=false`.
Для valid-слота адрес выровнен на размер строки, dirty — состояние именно
этой линии после всех переносов. В непрерывном exclusive-сегменте копия
строки одна; inclusive/NINE допускают дубли. Значения байтов и внутренности
policy не выдаются.

`EventOutcome.level_results` описывает demand probes до последующих переносов:
L2 hit может ссылаться на way, который уже освобождён после продвижения.
Miss с заполнением содержит way наблюдавшейся вставки и сведения о жертве;
неразмещающий miss имеет way=null, в том числе в L1 при NWA. `evictions` перечисляет жертвы всех уровней в порядке
каскадов. Вставки внутренних переносов во все нижние ways здесь не описаны,
поэтому восстанавливать физический снимок из outcomes нельзя. Источник
истины для слотов — state.

Семантика счётчиков совпадает с
[существующей статистикой](STATISTICS_FORMAT.md), с отдельным HTTP-кодированием:

- `summary.processed` и `totals` — проекция L1;
- `levels[].processed.trace_events` — число событий, которые сделали хотя бы
  один demand probe на данном уровне; line_accesses — число таких probes;
- hits+misses равны line_accesses; read/write определяются исходным kind
  события. Внутренние переносы и записи жертв не считаются demand probes;
- evictions — все фактические вытеснения этого уровня, включая каскады;
  writebacks — dirty-передачи вниз, включая слив dirty при promotion в WT и
  inclusive-инвалидации. Это не обязательно запись в память: dirty-линия из L1 может остаться в L2;
- `memory.reads` — fetch строк из памяти; `memory.writes` — все записи в память,
  `writebacks` — их подмножество с dirty-строками, `write_bytes` — объём записи;
  `requests` считает исходные события и запрошенные байты;
- hit_rate_percent — 100*hits/(hits+misses), null при отсутствии probes;
- инструкции содержат накопления по всем уровням и все объявленные sites,
  включая ещё не выполненные с нулями. Строки сортируются по **численному**
  access_id, а не лексикографически;
- `last_event` имеет sequence=cursor−1, при cursor=0 равен null. На EOF
  caches не flush-ятся, dirty-линии могут оставаться резидентными.

State содержит общую summary и last_event, но не таблицу всех инструкций.
Таблица читается отдельно. `set_offset` по умолчанию 0, `offset` инструкций
по умолчанию `"0"`; set_limit и limit обязательны. Конец страницы обрезается
по размеру набора, next_* равен null на последней странице. Offset, равный
размеру набора, возвращает пустую страницу, больший offset — 400.
Размер страницы положителен и не больше соответствующего negotiated limit.
Неизвестный уровень тоже даёт 400.

Лимиты `max_step_count`, `max_sets_per_read`, `max_instructions_per_read`
публикуются в Session, не задают предел длины всей трассы и не имеют
закреплённых контрактом значений. Backend не обязан держать всю трассу и
все результаты в памяти; память может зависеть от metadata, caches,
статистики, ограниченного batch и размера одного события.

## Metadata и представление чисел

Metadata содержит полный producer, trace status, buffer layout и static
access table. `source_map` присутствует только если он есть в источнике;
пустая выдуманная карта не подставляется. Сохраняются порядок и состав stages,
origins, origin_ids, stage_locations и fingerprint. Связь с outcomes и
статистикой идёт по access_id. Для нескольких origins/locations сохраняются
все связи. Stage paths относительны исходной директории трассы. В этом API
нет получения текста MLIR или произвольных файлов по stage path.

HTTP DTO отделены от файловых схем. `source_schema` указывает формат
источника, но преобразованный объект не объявляется артефактом с прежним
полем schema. Producer сохраняется целиком; `trace_event` содержит
нормализованные обязательные поля, не обещает сохранения всех input extensions.

| Значение | HTTP-представление |
|---|---|
| access_id, memref_id, origin_id, sequence, cursor, revision, counters, seed, размер события, offset страницы инструкций | Каноническая десятичная строка uint64: 0..18446744073709551615 |
| byte_offset, min_offset | Каноническая десятичная строка int64: −9223372036854775808..9223372036854775807 |
| model/base/line/evicted address | Каноническая lowercase hex-строка uint64, например `"0x10000000"` |
| bounded geometry, индексы level/set/way, count, executed_count, лимиты, размеры страниц, source line/column | JSON integer 0..9007199254740991; положительные там, где требует схема |
| hit_rate_percent | JSON number или null |

У десятичных строк нет плюса, ведущих нулей и `-0`; у hex — `0x`, без ведущих
нулей, кроме `0x0`, не более 16 цифр. Границы разрядности нормативны:
custom formats `uint64-decimal`/`int64-decimal` не каждый JSON Schema validator
проверяет самостоятельно. UI сохраняет строки или использует BigInt;
преобразование в Number допустимо только после проверки безопасного диапазона.
Query-параметры передаются текстом, OpenAPI указывает их логические типы.
Метаданные source line начинаются с 1, column — с 0.

Существующие `cachescope.trace.v1`, `simulation-statistics.v1` и
`simulation-events.v1` и их числовые типы не меняются. Для экспорта HTTP DTO
в файл нужен явный адаптер, а не смена schema-label.

## Ошибки и восстановление

Общий ответ: `{"error":{"code":"…","message":"…","details":{}},
"session_id":"…","revision":"…"}`. Session ID/revision включаются,
когда известен затронутый ресурс. Message предназначен человеку, code —
клиентской логике; details содержит диагностику без стабильной структуры.

| HTTP | Ситуации |
|---|---|
| 400 | invalid_request, invalid_config, invalid_count, invalid_page; состояние не меняется |
| 404 | session_not_found: нет ресурса, он удалён или потерян после перезапуска |
| 409 | session_exists, revision_mismatch, session_not_ready, session_busy, session_failed; запрошенное исполнение не происходит |
| 500 | execution_failed, trace_changed, internal_error; затронутая сессия переходит в failed |

Ошибки подготовки `trace_unreadable`/`invalid_trace` находятся в Session.error
при GET 200: 202 уже зарезервировал ресурс. Error сессии в failed содержит
`recovery`: reset разрешает reset или DELETE/create; recreate требует
DELETE/create. Ошибка подготовки и trace_changed всегда требуют recreate.
Ошибка исполнения позволяет reset; неуспешный reset оставляет failed.
Неожиданная ошибка до создания ресурса может вернуть 500 без session_id.

При сбое посреди batch частичная иерархия не становится допустимым снимком:
cursor указывает последний полностью завершённый prefix, статус failed,
revision увеличена, state/instructions недоступны с 409. Ответ 500 не
содержит успешного частичного StepResponse; откат модели не гарантируется.
После reset обнуляются также счётчики частично выполненного запроса.

Проверки формы, лимитов, состояния и revision происходят до запрошенного
исполнения. При нескольких одновременно нарушенных условиях порядок выбора
диагностики не закреплён. Ограничения арифметики проверяются; переполнение
счётчиков/revision не должно молча обнулять значения.

Новые optional поля событий: `back_invalidations` и `writebacks` имеют формат
`evictions`; `memory_writes` и `memory_write_bytes` — uint64 decimal strings.
`totals.back_invalidations` также decimal string. `way` может быть null на
любом NWA miss; при заполнении это наблюдавшийся слот. Служебная инвалидация
может удалить его позже в том же событии. Физическое состояние читайте через
`state`, а не восстанавливайте из outcomes. Старые запросы остаются допустимы.
