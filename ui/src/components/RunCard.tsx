import type { Run } from "../types";
import { fmtPct, ratio } from "../metrics";
import { HitMissBar } from "./HitMissBar";

function Metric({ label, value }: { label: string; value: string }) {
  return (
    <div className="metric">
      <div className="metric-value">{value}</div>
      <div className="muted">{label}</div>
    </div>
  );
}

export function RunCard({ run }: { run: Run }) {
  const s = run.stats;
  const t = s.totals;
  const n = s.processed.line_accesses;

  const rows: [string, number][] = [
    ["trace events", s.processed.trace_events],
    ["line accesses", n],
    ["read line accesses", t.read_line_accesses],
    ["write line accesses", t.write_line_accesses],
    ["hits", t.hits],
    ["misses", t.misses],
    ["read hits", t.read_hits],
    ["read misses", t.read_misses],
    ["write hits", t.write_hits],
    ["write misses", t.write_misses],
    ["evictions", t.evictions],
  ];

  return (
    <section className="card">
      <header>
        <h3>{s.variant}</h3>
        <span className="muted">{run.path}</span>
      </header>
      <p className="muted">producer: {s.producer}</p>

      {run.warnings.length > 0 && (
        <ul className="warn">
          {run.warnings.map((w) => (
            <li key={w}>⚠ {w}</li>
          ))}
        </ul>
      )}

      <div className="metrics">
        <Metric label="hit rate" value={fmtPct(ratio(t.hits, n))} />
        <Metric label="miss rate" value={fmtPct(ratio(t.misses, n))} />
      </div>

      <HitMissBar totals={t} />

      <dl className="totals">
        {rows.map(([k, v]) => (
          <div key={k}>
            <dt>{k}</dt>
            <dd>{v.toLocaleString("ru-RU")}</dd>
          </div>
        ))}
      </dl>
    </section>
  );
}
