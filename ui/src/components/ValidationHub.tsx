import { CACHE_PRESETS } from "../cachePresets";

const repository = "https://github.com/litvinovmitch11/CacheScope";

const validations = [
  {
    tool: "Valgrind Cachegrind",
    result: "25 / 25 префиксов совпали",
    scope: "L1D: reads, writes, hits и misses на трёх контролируемых native sequences.",
    environment: "Valgrind 3.22.0 · GCC 13.3.0 · Ubuntu 24.04 ARM64",
    command: "python3 scripts/compare_cachegrind.py --output /tmp/cachegrind-results",
    docs: `${repository}/blob/main/docs/CACHEGRIND_COMPARISON.md`,
    workflow: `${repository}/actions/workflows/cachegrind.yml`,
  },
  {
    tool: "DynamoRIO drcachesim",
    result: "19 + 45 + 49 префиксов совпали",
    scope: "L1D и двух-/трёхуровневая exclusive LRU hierarchy на одинаковых адресах.",
    environment: "DynamoRIO 11.91.20763 · Ubuntu 24.04 ARM64",
    command: "python3 scripts/compare_drcachesim.py --dynamorio /path/to/DynamoRIO --output /tmp/drcachesim-results",
    docs: `${repository}/blob/main/docs/DRCACHESIM_COMPARISON.md`,
    workflow: `${repository}/actions/workflows/drcachesim.yml`,
  },
  {
    tool: "DynamoRIO drmemtrace",
    result: "6 событий + 3 × 16 384 событий совпали",
    scope: "Semantic trace сопоставлен с machine trace: kind, buffer, offset, size и порядок.",
    environment: "LLVM/MLIR 22.1.8 · DynamoRIO 11.91.20763 · clang -O0",
    command: "python3 tests/compare_drmemtrace_matmul.py --mlir-bin /usr/lib/llvm-22/bin --plugin build/lib/CacheScopePassPlugin.so --runtime build/compiler/runtime/libcachescope_trace_runtime.so --dynamorio /path/to/DynamoRIO",
    docs: `${repository}/blob/main/docs/DRMEMTRACE_COMPARISON.md`,
    workflow: `${repository}/actions/workflows/cache-pipeline.yml`,
  },
] as const;

export function ValidationHub() {
  return (
    <section className="validation-hub">
      <div className="validation-hero">
        <div>
          <p className="product-eyebrow">INDEPENDENT VALIDATION</p>
          <h2>Почему результатам cache model можно доверять</h2>
          <p>
            CacheScope проверяется не только собственными unit-тестами: те же
            обращения независимо воспроизводятся Cachegrind и DynamoRIO, а
            semantic trace сопоставляется с трассой реально выполненного
            машинного кода.
          </p>
        </div>
        <a className="button-link primary-report-action" href="/?open=hardware-presets#hardware-presets">Выбрать hardware preset →</a>
      </div>

      <div className="validation-boundary">
        <strong>Что именно доказано</strong>
        <span>Корректность cache transitions на контролируемых трассах</span>
        <span>Совпадение semantic и machine memory accesses</span>
        <span>Воспроизводимая методика и CI workflows</span>
        <strong>Что не заявляется</strong>
        <span>Cycle accuracy, wall-clock speedup, PMU correlation, prefetch и speculation</span>
      </div>

      <div className="validation-grid">
        {validations.map((item) => (
          <article className="validation-card" key={item.tool}>
            <div className="validation-card-heading">
              <div><p className="product-eyebrow">REFERENCE CHECK</p><h3>{item.tool}</h3></div>
              <span>matched</span>
            </div>
            <strong className="validation-result">{item.result}</strong>
            <p>{item.scope}</p>
            <small>{item.environment}</small>
            <details>
              <summary>Воспроизвести локально</summary>
              <code>{item.command}</code>
            </details>
            <div className="validation-links">
              <a href={item.docs} target="_blank" rel="noreferrer">Методика</a>
              <a href={item.workflow} target="_blank" rel="noreferrer">CI workflow</a>
            </div>
          </article>
        ))}
      </div>

      <section className="hardware-catalog">
        <div>
          <p className="product-eyebrow">DOCUMENTED CPU MODELS</p>
          <h2>Пресеты реального железа</h2>
          <p>В каталог входят только конфигурации, для которых подтверждены все показываемые параметры.</p>
        </div>
        <div className="hardware-cards">
          {CACHE_PRESETS.map((preset) => (
            <article key={preset.id}>
              <h3>{preset.name}</h3>
              <p>{preset.description}</p>
              <ul>{preset.sources.map((source) => <li key={source.url}><a href={source.url} target="_blank" rel="noreferrer">{source.title}</a></li>)}</ul>
            </article>
          ))}
        </div>
      </section>
    </section>
  );
}
