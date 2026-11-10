import type { Run } from "../types";
import { fmtDeltaPp, fmtPct, fmtSigned, ratio } from "../metrics";

/** Сравнение запусков с одинаковой конфигурацией кэша. База — variant "baseline" или первый. */
export function ComparisonTable({ runs }: { runs: Run[] }) {
  const base = runs.find((r) => r.stats.variant === "baseline") ?? runs[0];
  const baseHit = ratio(base.stats.totals.hits, base.stats.processed.line_accesses);

  return (
    <table className="cmp">
      <thead>
        <tr>
          <th>variant</th>
          <th>line accesses</th>
          <th>hit rate</th>
          <th>Δ hit rate</th>
          <th>misses</th>
          <th>Δ misses</th>
          <th>evictions</th>
        </tr>
      </thead>
      <tbody>
        {runs.map((r) => {
          const t = r.stats.totals;
          const n = r.stats.processed.line_accesses;
          const hit = ratio(t.hits, n);
          const isBase = r === base;
          return (
            <tr key={r.path} className={isBase ? "base" : undefined}>
              <td>
                {r.stats.variant}
                {isBase && <span className="muted"> (база)</span>}
              </td>
              <td>{n.toLocaleString("ru-RU")}</td>
              <td>{fmtPct(hit)}</td>
              <td>{isBase ? "—" : fmtDeltaPp(hit, baseHit)}</td>
              <td>{t.misses.toLocaleString("ru-RU")}</td>
              <td>{isBase ? "—" : fmtSigned(t.misses - base.stats.totals.misses)}</td>
              <td>{t.evictions.toLocaleString("ru-RU")}</td>
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}
