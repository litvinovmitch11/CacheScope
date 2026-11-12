# Проверка профилей процессоров

В UI допускаются только профили с подтверждённым совпадением **всех** настроек:
размер, размер строки, ways, замещение, запись, размещение при промахе записи
и связь между уровнями. Для каждого уровня нужны ссылки на документацию
конкретной модели. Неподтверждённое значение или аппаратная политика, которую
модель заменяет приближением, исключают профиль из выбора.

## Подтверждённые профили встроенного L1

Профили описывают **встроенный объединённый L1** IntelDX2 и Write-Back Enhanced
IntelDX4. Выбрана однопроцессорная система без внешнего кэша: в [рис. 3-9 Intel](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=65)
внешний кэш необязателен. Поэтому в профиле один уровень и нет
inclusive/exclusive/NINE-связи; `hierarchy.inclusion_policies=[]`.

| Параметр | IntelDX2 | Write-Back Enhanced IntelDX4 | Основание |
|---|---|---|---|
| Размер L1 | 8192 B, 128 sets | 16384 B, 256 sets | [§7.1–7.1.2, рис. 7-1](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=179) |
| Строка | 16 B | 16 B | [§7.1, рис. 7-1](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=179) |
| Ways | 4 | 4 | [§7.1–7.1.2](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=179) |
| Замещение | четырёхканальный tree-PLRU (`PLRU4`) | тот же tree-PLRU | [§3.2.3](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=57), [§7.5, рис. 7-2](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=186) |
| Запись | write-through | write-back для выбранных E/M-линий | [DX2: §7.1](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=180), [DX4: §7.1.2](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=181), [табл. 7-6](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=194) |
| Промах записи | no-write-allocate | no-write-allocate, даже в WB-режиме | [DX2: §7.1](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=180), [DX4: табл. 7-6, строка I](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=194) |
| Связь уровней | неприменима: только встроенный L1 | неприменима: только встроенный L1 | [рис. 3-9](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=65) |

Для DX2 включено обычное кэширование. Для DX4 явно выбран Enhanced Bus Mode,
WB/WT#=1 и PWT=0 для заполняемых кэшируемых линий: [табл. 7-5](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=193).
S-линии, snoop, cache-disable и смена атрибутов памяти не входят в этот профиль.
Трасса содержит обращения к данным; выборки команд из аппаратного unified L1,
другие bus masters, буферизация и задержки шин не воспроизводятся. Это
подтверждённые параметры L1 в указанном режиме, а не полная модель ПК.

В реализации используется отдельный `PLRU4`, а не LRU. Проверки покрывают
все восемь состояний дерева Intel и трассу, на которой PLRU и LRU выбирают
разных victims. UI/HTTP/CLI требуют ровно четыре ways для этой политики.

## Ранее исключённые кандидаты

Ниже сохранены причины исключения прежних семи конфигураций. Их значения
не возвращаются в каталог. Новый профиль DX2/DX4 использует документированный
PLRU; старый профиль Intel486 DX использовал LRU и остаётся исключённым.

| Удалённый профиль | Причина исключения | Источник |
|---|---|---|
| AMD Phenom II X4 955, L1D/L2 | В профиле L2 были заданы LRU и write-allocate без подтверждения для этой модели. Руководство подтверждает L1 LRU/WB/WA и exclusive L2, но оставляет размер/ways L2 зависимыми от реализации; Family 10h также имеет L3, отсутствовавший в профиле. | [AMD 40546, §A.5.2–A.5.4, PDF с. 246–247](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/software-optimization-guides/40546.pdf#page=246) |
| AMD Opteron 2384, L1D/L2 | Та же неподтверждённая политика L2 и опущенный L3. Семейный документ не подтверждает полный набор параметров конкретной модели. | [AMD 40546, §A.5.2–A.5.4](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/software-optimization-guides/40546.pdf#page=246) |
| AMD A8-3850, Llano, L1D/L2 | LRU и write-allocate L2 были допущениями. Подтверждения L1 и exclusive L2 недостаточно для полного профиля. | [AMD 40546, §A.5.2–A.5.3](https://www.amd.com/content/dam/amd/en/documents/archived-tech-docs/software-optimization-guides/40546.pdf#page=246) |
| MIPS R10000, только L1D | **Ошибка:** размер строки был 64 B, документация прямо задаёт 32 B для primary data cache (64 B относится к instruction cache). Inclusive L2 отсутствовал; write-allocate не был подтверждён. | [MIPS R10000 User’s Manual, Cache Instructions / Virtual Address, PDF с. 218](https://file.wehack.space/vimacs/MIPS_R10000_UM.pdf#page=218), [§4.2–4.3, PDF с. 78–81](https://file.wehack.space/vimacs/MIPS_R10000_UM.pdf#page=78) |
| Intel Pentium II, L1D/L2 | Источник относится к Mobile Pentium II 233/266/300 MHz, а профиль не уточнял модель. Документ подтверждает часть геометрии и WB L1D, но не выбранные LRU и NINE. | [Intel Mobile Pentium II Datasheet 243669-002, титульная страница и §1.2](https://datasheets.chipdb.org/Intel/x86/Pentium%20II/24366902.PDF) |
| Intel Pentium III, Coppermine, L1D/L2 | Выбранные LRU и NINE не подтверждены product brief; сам источник описывает Low Power-варианты и не обосновывает полный профиль. | [Intel Pentium III Low Power Product Brief 273375-003](https://download.intel.com/design/intarch/prodbref/27337503.pdf) |
| Intel486 DX, только L1 | **Несовпадение замещения:** в профиле LRU, аппаратно pseudo-LRU с тремя битами на set. Размер/строка/ways и WT/NWA не исправляют это расхождение. Аппаратный L1 также общий для кода и данных. | [Intel486 Developer’s Manual 273021-001, §7.5, PDF с. 186](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=186), [§7.1, PDF с. 179–180](https://bitsavers.trailing-edge.com/components/intel/80486/273021-001_Embedded_Intel486_Processor_Family_Developers_Manual_199710.pdf#page=179) |

Проверка каждого исключённого кандидата прекращается после выявленного основания для
исключения: остальные поля не объявляются полностью проверенными. Сохранённые
эксперименты с прежними `cache_profile` остаются доступны как ручные
конфигурации: используются их `levels` и `hierarchy`, без названия процессора.
Тесты runner и UI подтверждают поведение модели; они не подтверждают совпадение
с характеристиками аппаратного процессора.
