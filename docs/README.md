# Документация CacheScope

## Пользователю

- [Запуск и основной UI-сценарий](../README.md#быстрый-запуск)
- [Все инструменты командной строки](CLI.md)
- [Формат Torch project](../README.md#формат-редактируемого-проекта)
- [Поддерживаемые операции и ограничения](SUPPORT_MATRIX.md)
- [Модели кеша и правила сравнения](CACHE_MODELS.md)

## Архитектура и API

- [Общая архитектура compiler/runtime/simulator](ARCHITECTURE.md)
- [Архитектура локального приложения](WEB_APP_ARCHITECTURE.md)
- [Runner HTTP API](RUNNER_API.md)
- [Project и MLIR experiment API](EXPERIMENT_API.md)
- [Trace format](TRACE_FORMAT.md)
- [Source mapping](SOURCE_MAPPING.md)
- [Simulation results](SIMULATION_FORMAT.md)
- [Statistics extensions](STATISTICS_FORMAT.md)

## Доказательная база

- [Критерии корректности](CORRECTNESS_VALIDATION.md)
- [Valgrind Cachegrind](CACHEGRIND_COMPARISON.md)
- [DynamoRIO drcachesim](DRCACHESIM_COMPARISON.md)
- [DynamoRIO drmemtrace](DRMEMTRACE_COMPARISON.md)
- [Аудит hardware presets](PROCESSOR_PRESET_AUDIT.md)

Внешние инструменты подтверждают модель и semantic trace в перечисленных
условиях. Они не являются hardware benchmark или PMU validation.

## Разработка

- [Development guide](DEVELOPMENT.md)
- [Интеграция компонентов](INTEGRATION.md)
- [Cache pipeline validation](CACHE_PIPELINE_VALIDATION.md)
- [План развития после alpha](../ROADMAP.md)

Reference drivers в `examples/` нужны для regression и интеграции отдельных
компонентов. Основной пользовательский путь — локальный Docker Project Studio.
