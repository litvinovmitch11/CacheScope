import type { Run, Statistics } from "./types";

/** Доля n/d или null, если делить нельзя (line_accesses == 0). */
export function ratio(n: number, d: number): number | null {
  return d === 0 ? null : n / d;
}

/** 0.6667 -> "66.67%", null -> "—" */
export function fmtPct(r: number | null): string {
  return r === null ? "—" : (r * 100).toFixed(2) + "%";
}

/** Разница в процентных пунктах: "+12.34 п.п." */
export function fmtDeltaPp(r: number | null, base: number | null): string {
  if (r === null || base === null) return "—";
  const d = (r - base) * 100;
  return (d > 0 ? "+" : "") + d.toFixed(2) + " п.п.";
}

export function fmtSigned(n: number): string {
  return (n > 0 ? "+" : "") + n.toLocaleString("ru-RU");
}

export function describeConfig(c: Statistics["cache_config"]): string {
  const size =
    c.size_bytes % 1024 === 0 ? `${c.size_bytes / 1024} KiB` : `${c.size_bytes} B`;
  return `${size} · ${c.line_size_bytes} B line · ${c.associativity}-way · ${c.set_count} sets · ${c.replacement_policy}`;
}

/** Обязательные соотношения из SIMULATION_FORMAT.md. Пустой массив = всё ок. */
export function checkInvariants(s: Statistics): string[] {
  const { totals: t, processed: p, cache_config: c } = s;
  const errs: string[] = [];
  if (t.hits + t.misses !== p.line_accesses)
    errs.push("hits + misses ≠ line_accesses");
  if (t.read_line_accesses + t.write_line_accesses !== p.line_accesses)
    errs.push("read_line_accesses + write_line_accesses ≠ line_accesses");
  if (t.read_hits + t.write_hits !== t.hits)
    errs.push("read_hits + write_hits ≠ hits");
  if (t.read_misses + t.write_misses !== t.misses)
    errs.push("read_misses + write_misses ≠ misses");
  if (c.size_bytes !== c.line_size_bytes * c.associativity * c.set_count)
    errs.push("size_bytes ≠ line_size × associativity × set_count");
  return errs;
}

/** Группирует запуски с одинаковой конфигурацией кэша: только их можно сравнивать. */
export function groupByConfig(runs: Run[]): Run[][] {
  const groups = new Map<string, Run[]>();
  for (const r of runs) {
    const c = r.stats.cache_config;
    const key = [
      c.size_bytes,
      c.line_size_bytes,
      c.associativity,
      c.set_count,
      c.replacement_policy,
    ].join("|");
    groups.set(key, [...(groups.get(key) ?? []), r]);
  }
  return [...groups.values()];
}
