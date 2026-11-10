import { useState } from "react";
import type { Run } from "../types";
import {
  describeConfig,
  describeHierarchy,
  fmtCount,
  fmtPct,
  getLevels,
  ratio,
} from "../metrics";
import { HitMissBar } from "./HitMissBar";
import { LevelTabs } from "./LevelTabs";
import { InstructionsTable } from "./InstructionsTable";

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
  const levels = getLevels(s);
  const [sel, setSel] = useState(levels[0].level);
  const cur = levels.find((l) => l.level === sel) ?? levels[0];
  const t = cur.totals;
  const n = cur.processed.line_accesses;
  const policy = describeHierarchy(s);

  const rows: [string, string][] = [
    ["trace events", fmtCount(cur.processed.trace_events)],
    ["line accesses", fmtCount(n)],
    ["read line accesses", fmtCount(t.read_line_accesses)],
    ["write line accesses", fmtCount(t.write_line_accesses)],
    ["hits", fmtCount(t.hits)],
    ["misses", fmtCount(t.misses)],
    ["read hits", fmtCount(t.read_hits)],
    ["read misses", fmtCount(t.read_misses)],
    ["write hits", fmtCount(t.write_hits)],
    ["write misses", fmtCount(t.write_misses)],
    ["evictions", fmtCount(t.evictions)],
    ["writebacks", fmtCount(t.writebacks)],
  ];

  return (
    <section className="card">
      <header>
        <h3>{s.variant}</h3>
        <span className="muted">{run.path}</span>
      </header>
      <p className="muted">
        producer: {s.producer}
        {policy && ` · ${policy}`}
      </p>
      {run.trace ? (
        <p className={run.trace.truncated || !run.trace.valid ? "warn" : "muted"}>
          trace: {run.trace.recorded_events.toLocaleString("ru-RU")} из{" "}
          {run.trace.total_events.toLocaleString("ru-RU")} событий
          {run.trace.truncated ? " · УСЕЧЁН" : ""}
          {!run.trace.valid ? " · НЕВАЛИДНА" : ""}
        </p>
      ) : (
        <p className="muted">
          Заголовок trace недоступен; статистика симуляции загружена отдельно.
        </p>
      )}

      {run.warnings.length > 0 && (
        <ul className="warn">
          {run.warnings.map((w) => (
            <li key={w}>⚠ {w}</li>
          ))}
        </ul>
      )}

      {/* Сводка по всем уровням. Проценты локальные, уровни не суммируются. */}
      <table className="cmp small levels">
        <thead>
          <tr>
            <th>уровень</th>
            <th>line acc.</th>
            <th>hit rate</th>
            <th>misses</th>
            <th>evictions</th>
            <th>writebacks</th>
          </tr>
        </thead>
        <tbody>
          {levels.map((l) => (
            <tr
              key={l.level}
              className={l.level === cur.level ? "sel" : undefined}
              onClick={() => setSel(l.level)}
            >
              <td>{l.name}</td>
              <td>{fmtCount(l.processed.line_accesses)}</td>
              <td>{fmtPct(ratio(l.totals.hits, l.processed.line_accesses))}</td>
              <td>{fmtCount(l.totals.misses)}</td>
              <td>{fmtCount(l.totals.evictions)}</td>
              <td>{fmtCount(l.totals.writebacks)}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {levels.length > 1 && (
        <p className="muted">
          hit rate локальный: hits уровня / его собственные обращения. Уровни не суммируются.
        </p>
      )}

      {s.memory && (
        <p className="muted">
          Память: чтений строк {fmtCount(s.memory.reads)}, writebacks {fmtCount(s.memory.writebacks)}
        </p>
      )}

      <LevelTabs levels={levels} value={cur.level} onChange={setSel} />
      <p className="muted">
        {cur.name}: {describeConfig(cur.cache_config)}
      </p>

      <div className="metrics">
        <Metric label="hit rate" value={fmtPct(ratio(t.hits, n))} />
        <Metric label="miss rate" value={fmtPct(ratio(t.misses, n))} />
      </div>

      <HitMissBar totals={t} />

      <dl className="totals">
        {rows.map(([k, v]) => (
          <div key={k}>
            <dt>{k}</dt>
            <dd>{v}</dd>
          </div>
        ))}
      </dl>

      <InstructionsTable instructions={s.instructions} level={cur.level} />
    </section>
  );
}


