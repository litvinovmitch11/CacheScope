# CacheScope live demo examples

Эта директория содержит два исходных MLIR payload и шесть реально
компилируемых вариантов. Она предназначена для показа compiler-side идеи до
готовности instrumentation pass, cache simulator и UI.

[`cache-demo-1k.yaml`](cache-demo-1k.yaml) — намеренно маленькая учебная
модель кеша для будущего replay коротких трасс `16×16`. Она нужна, чтобы
разница обходов была заметна визуально, и не изображает кеш реального CPU.

## Быстрый запуск

Из корня репозитория:

```bash
chmod +x examples/live-demo/run-demo.sh
examples/live-demo/run-demo.sh
```

По умолчанию используются инструменты из `llvm-project/build/bin`. Для другой
сборки LLVM/MLIR:

```bash
MLIR_BIN_DIR=/path/to/llvm-build/bin examples/live-demo/run-demo.sh
```

Скрипт для каждого варианта выполняет четыре настоящих шага:

```text
payload
  → Transform Dialect schedule
  → lowering linalg в scf/memref loops
  → lowering в LLVM dialect
  → JIT execution через mlir-runner
```

## Сюжет 1: matmul

Один payload: [`matmul.mlir`](matmul.mlir).

Варианты:

1. `matmul-baseline` — обычный порядок `i → j → k`;
2. `matmul-tile4` — блоки `4×4×4`, порядок внешних блоков `i → j → k`;
3. `matmul-tile4-ikj` — те же блоки, но порядок `i → k → j`.

Все три варианта возвращают `16`, то есть вычисляют одинаковый результат.
Transform scripts:

- [`matmul-tile4.transform.mlir`](matmul-tile4.transform.mlir);
- [`matmul-tile4-ikj.transform.mlir`](matmul-tile4-ikj.transform.mlir).

## Сюжет 2: copy

Один payload: [`copy.mlir`](copy.mlir).

Варианты:

1. `copy-baseline` — простой построчный обход;
2. `copy-tile4` — обход блоками `4×4`;
3. `copy-tile4-column-first` — те же блоки, но сначала меняется координата
   столбцового блока.

Все три варианта возвращают `7`.

## Какие файлы показывать

После запуска появится `generated/`:

- `*.transformed.mlir` — результат конкретной Transform Dialect strategy;
- `*.loops.mlir` — читаемые циклы и явные `memref.load/store`;
- `*.llvm.mlir` — результат lowering до LLVM dialect.

Для live demo удобнее открыть рядом:

```text
matmul.mlir
matmul-tile4-ikj.transform.mlir
generated/matmul-baseline.loops.mlir
generated/matmul-tile4-ikj.loops.mlir
```

В `*.loops.mlir` уже видны будущие точки инструментации. Перед каждым
поддерживаемым `memref.load` и `memref.store` instrumentation pass добавит
вызов `__cachescope_access`. Исследуемые вычисления специально вынесены в
`@matmul_kernel` и `@copy_kernel`: инициализация в `@main` не должна попадать в
cache trace. Дальше можно словами показать следующий ещё не реализованный
участок pipeline:

```text
instrumented loops
  → tracing runtime
  → trace.v1.json
  → cache simulator
  → run.v1.json
  → UI replay
```

## Рекомендуемый порядок показа

1. Показать один исходный `matmul.mlir`.
2. Показать короткий transform script с tile sizes и interchange.
3. Запустить `run-demo.sh` и обратить внимание, что все варианты дают один
   математический результат.
4. Сравнить baseline и transformed `*.loops.mlir`.
5. Найти `memref.load/store` и объяснить место будущей инструментации.
6. Нарисовать продолжение: trace → simulator → UI.

`generated/` является воспроизводимым результатом компиляции и не должен
редактироваться вручную.
