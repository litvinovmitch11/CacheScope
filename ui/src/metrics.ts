import type {
  CacheConfig,
  Processed,
  Run,
  Statistics,
  Totals,
  TraceSummary,
} from "./types";

/** Единое представление одного уровня (для single-cache строится из верхнего уровня). */
export interface LevelView {
  level: number;
  name: string;
  cache_config: CacheConfig;
  processed: Processed;
  totals: Totals;
}

/** Уровни по возрастанию `level`. Нет `levels` — один уровень L1 из верхнеуровневых полей. */
export function getLevels(s: Statistics): LevelView[] {
  if (s.levels && s.levels.length > 0) {
    return [...s.levels].sort((a, b) => a.level - b.level);
  }
  return [
    {
      level: 1,
      name: "L1",
      cache_config: s.cache_config,
      processed: s.processed,
      totals: s.totals,
    },
  ];
}

/** Доля n/d или null, если делить нельзя (нет обращений). */
export function ratio(n: number, d: number): number | null {
  return d === 0 ? null : n / d;
}

export function fmtPct(r: number | null): string {
  return r === null ? "—" : (r * 100).toFixed(2) + "%";
}

export function fmtDeltaPp(r: number | null, base: number | null): string {
  if (r === null || base === null) return "—";
  const d = (r - base) * 100;
  return (d > 0 ? "+" : "") + d.toFixed(2) + " п.п.";
}

export function fmtSigned(n: number): string {
  return (n > 0 ? "+" : "") + n.toLocaleString("ru-RU");
}

/** Счётчик, который может быть null/отсутствовать: тогда «—», а не 0. */
export function fmtCount(n: number | null | undefined): string {
  return n == null ? "—" : n.toLocaleString("ru-RU");
}

export function describeConfig(c: CacheConfig): string {
  const size =
    c.size_bytes % 1024 === 0 ? `${c.size_bytes / 1024} KiB` : `${c.size_bytes} B`;
  return `${size} · ${c.line_size_bytes} B line · ${c.associativity}-way · ${c.set_count} sets · ${c.replacement_policy}${c.write_policy ? " · " + c.write_policy : ""}${c.allocation_policy ? " · " + c.allocation_policy : ""}`;
}

export function describeHierarchy(s: Statistics): string {
  const h = s.hierarchy;
  if (!h) return "";
  const links = h.inclusion_policies?.map((policy, i) => `L${i + 1}/L${i + 2}: ${policy}`).join(", ");
  const inclusion = h.inclusion_policies?.length === 0 ? "один уровень" : links || h.inclusion_policy;
  return [inclusion, h.write_policy, h.allocation_policy,
    h.inclusion_policies?.length === 0 || h.writeback_clean === undefined ? null : `clean victims: ${h.writeback_clean ? "forward" : "drop (except exclusive)"}`,
    h.inclusion_policies?.length !== 0 && h.non_inclusive_fill ? `NINE fill: ${h.non_inclusive_fill}` : null]
    .filter((x): x is string => !!x)
    .join(" · ");
}

export function describeLevels(levels: LevelView[]): string {
  if (levels.length === 1) return describeConfig(levels[0].cache_config);
  return levels.map((l) => `${l.name} ${describeConfig(l.cache_config)}`).join("  →  ");
}

const BASE_TOTALS = [
  "read_line_accesses",
  "write_line_accesses",
  "hits",
  "misses",
  "read_hits",
  "read_misses",
  "write_hits",
  "write_misses",
  "evictions",
] as const;

function sameConfig(a: CacheConfig, b: CacheConfig): boolean {
  return (
    a.size_bytes === b.size_bytes &&
    a.line_size_bytes === b.line_size_bytes &&
    a.associativity === b.associativity &&
    a.set_count === b.set_count &&
    a.replacement_policy === b.replacement_policy &&
    a.write_policy === b.write_policy && a.allocation_policy === b.allocation_policy
  );
}

/** Обязательные соотношения из раздела 1, применяются к каждому уровню. */
function checkCounters(p: Processed, t: Totals): string[] {
  const e: string[] = [];
  if (t.hits + t.misses !== p.line_accesses) e.push("hits + misses ≠ line_accesses");
  if (t.read_line_accesses + t.write_line_accesses !== p.line_accesses)
    e.push("read_line_accesses + write_line_accesses ≠ line_accesses");
  if (t.read_hits + t.write_hits !== t.hits) e.push("read_hits + write_hits ≠ hits");
  if (t.read_misses + t.write_misses !== t.misses)
    e.push("read_misses + write_misses ≠ misses");
  return e;
}

export function checkInvariants(s: Statistics): string[] {
  const errs: string[] = [];
  const levels = getLevels(s);

  if (new Set(levels.map((l) => l.level)).size !== levels.length) {
    errs.push("в levels повторяются номера level");
  }

  for (const l of levels) {
    for (const m of checkCounters(l.processed, l.totals)) errs.push(`${l.name}: ${m}`);
    const c = l.cache_config;
    if (c.size_bytes !== c.line_size_bytes * c.associativity * c.set_count)
      errs.push(`${l.name}: size_bytes ≠ line_size × associativity × set_count`);
  }

  // верхнеуровневые поля обязаны совпадать с L1 (раздел 6.1)
  if (s.levels && s.levels.length > 0) {
    const l1 = s.levels.find((l) => l.level === 1);
    if (!l1) {
      errs.push("в levels нет записи level = 1");
    } else {
      if (!sameConfig(l1.cache_config, s.cache_config))
        errs.push("верхнеуровневый cache_config ≠ levels[level=1]");
      if (
        l1.processed.trace_events !== s.processed.trace_events ||
        l1.processed.line_accesses !== s.processed.line_accesses
      )
        errs.push("верхнеуровневый processed ≠ levels[level=1]");
      if (BASE_TOTALS.some((k) => l1.totals[k] !== s.totals[k]))
        errs.push("верхнеуровневый totals ≠ levels[level=1]");
    }
  }
  return errs;
}

/** Проверки trace-шапки и её согласованности со статистикой симулятора. */
export function checkTrace(
  t: TraceSummary | null,
): string[] {
  if (!t) return [];

  const warnings: string[] = [];

  if (!t.valid) {
    warnings.push("Исходная trace помечена как невалидная");
  }

  if (t.truncated) {
    warnings.push(
      `trace усечён: записано ${t.recorded_events.toLocaleString("ru-RU")} ` +
      `из ${t.total_events.toLocaleString("ru-RU")} событий`,
    );
  }

  if (t.total_events !== t.recorded_events && !t.truncated) {
    warnings.push(
      "trace: total_events ≠ recorded_events, но truncated = false",
    );
  }

  if (t.complete !== (t.valid && !t.truncated)) {
    warnings.push(
      "trace: значение complete не соответствует valid и truncated",
    );
  }

  return warnings;
}

/** Запуски сравнимы, только если одинакова вся иерархия: конфиги уровней и политики. */
export function groupByConfig(runs: Run[]): Run[][] {
  const groups = new Map<string, Run[]>();
  for (const r of runs) {
    const key = JSON.stringify([
      getLevels(r.stats).map((l) => [
        l.level,
        l.cache_config.size_bytes,
        l.cache_config.line_size_bytes,
        l.cache_config.associativity,
        l.cache_config.set_count,
        l.cache_config.replacement_policy,
        l.cache_config.write_policy ?? null,
        l.cache_config.allocation_policy ?? null,
      ]),
      r.stats.hierarchy?.inclusion_policy ?? null,
      r.stats.hierarchy?.write_policy ?? null,
      r.stats.hierarchy?.allocation_policy ?? null,
      r.stats.hierarchy?.inclusion_policies ?? null,
      r.stats.hierarchy?.writeback_clean ?? null,
      r.stats.hierarchy?.non_inclusive_fill ?? null,
    ]);
    groups.set(key, [...(groups.get(key) ?? []), r]);
  }
  return [...groups.values()];
}
