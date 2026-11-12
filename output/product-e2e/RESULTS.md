# Результаты CacheScope product E2E

Иерархия: exclusive, write-back, write-allocate.

| Level | Capacity | Line | Ways | Policy |
|---|---:|---:|---:|---|
| `L1` | 512 B | 64 B | 1 | `LRU` |

## Сравнение L1

| Variant | Hits | Misses | Hit rate | Δ misses к baseline | Δ hit rate |
|---|---:|---:|---:|---:|---:|
| `baseline` | 4096 | 12288 | 25.00% | +0 | +0.00 pp |
| `tile4` | 7360 | 9024 | 44.92% | -3264 | +19.92 pp |
| `tile4-ikj` | 7504 | 8880 | 45.80% | -3408 | +20.80 pp |

Минимум промахов: `tile4-ikj` — 8880 (-3408 относительно baseline).
Hits и misses следует сравнивать отдельно для каждого уровня.

Автономный UI: [report.html](report.html).
