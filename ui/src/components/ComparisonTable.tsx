import { useState } from "react";
import type { Run } from "../types";
import { fmtCount, fmtDeltaPp, fmtPct, fmtSigned, getLevels, ratio } from "../metrics";
import { LevelTabs } from "./LevelTabs";

export function ComparisonTable({ runs }: { runs: Run[] }) {
  const base = runs.find((r) => r.stats.variant === "baseline") ?? runs[0];
  const baseLevels = getLevels(base.stats);
  const [lvl, setLvl] = useState(baseLevels[0].level);

  const levelOf = (r: Run) => getLevels(r.stats).find((l) => l.level === lvl);
  const baseL = levelOf(base);
  const baseHit = baseL ? ratio(baseL.totals.hits, baseL.processed.line_accesses) : null;

  const truncated = runs.filter((r) => r.trace?.truncated);
  const showMem = runs.some((r) => r.stats.memory);

  return (
    <>
      {truncated.length > 0 && (
        <p className="warn">
          ⚠ Trace усечён у: {truncated.map((r) => r.stats.variant).join(", ")}. У этих
          вариантов записан только префикс выполнения, счётчики неполные, поэтому
          сравнение некорректно.
        </p>
      )}

      <LevelTabs levels={baseLevels} value={lvl} onChange={setLvl} />
      {baseLevels.length > 1 && (
        <p className="muted">
          Показатели уровня считаются локально (по его собственным обращениям). Для нижних
          уровней на них влияет и то, сколько запросов дошло от верхних.
        </p>
      )}

      <table className="cmp">
        <thead>
          <tr>
            <th>variant</th>
            <th>trace events</th>
            <th>line accesses</th>
            <th>hit rate</th>
            <th>Δ hit rate</th>
            <th>misses</th>
            <th>Δ misses</th>
            <th>evictions</th>
            <th>writebacks</th>
            {showMem && <th>mem reads</th>}
            {showMem && <th>mem writebacks</th>}
          </tr>
        </thead>
        <tbody>
          {runs.map((r) => {
            const L = levelOf(r);
            const isBase = r === base;
            const n = L?.processed.line_accesses;
            const hit = L && n !== undefined ? ratio(L.totals.hits, n) : null;
            return (
              <tr key={r.path} className={isBase ? "base" : undefined}>
                <td>
                  {r.stats.variant}
                  {isBase && <span className="muted"> (база)</span>}
                </td>
                <td>{fmtCount(L?.processed.trace_events)}</td>
                <td>{fmtCount(n)}</td>
                <td>{fmtPct(hit)}</td>
                <td>{isBase ? "—" : fmtDeltaPp(hit, baseHit)}</td>
                <td>{fmtCount(L?.totals.misses)}</td>
                <td>
                  {isBase || !L || !baseL
                    ? "—"
                    : fmtSigned(L.totals.misses - baseL.totals.misses)}
                </td>
                <td>{fmtCount(L?.totals.evictions)}</td>
                <td>{fmtCount(L?.totals.writebacks)}</td>
                {showMem && <td>{fmtCount(r.stats.memory?.reads)}</td>}
                {showMem && <td>{fmtCount(r.stats.memory?.writebacks)}</td>}
              </tr>
            );
          })}
        </tbody>
      </table>
    </>
  );
}
