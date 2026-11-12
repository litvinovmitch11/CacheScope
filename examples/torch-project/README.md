# Редактируемые Torch projects

Эти директории можно открыть кнопкой «Открыть папку проекта» в локальном
Experiment Studio. Минимальный проект содержит `model.py`, `inputs.py` и хотя
бы один `transforms/*.mlir`; `cache-profiles.json` необязателен. Профили могут
быть массивами `NAME:SIZE:LINE:WAYS:POLICY` либо structured-конфигурациями
`{levels, hierarchy}` с write/allocation policies.

Готовый пример: `depthwise-temporal-blocking/`. Он содержит baseline и три
Transform Dialect schedules с тайлами `6`, `10` и `14`. Все четыре варианта
используют один набор детерминированных inputs; каждый compiled output
проверяется относительно eager Torch output до cache simulation.
