import type { Totals } from "../types";

const SEGMENTS = [
  { key: "read_hits", label: "read hit", cls: "seg-rh" },
  { key: "write_hits", label: "write hit", cls: "seg-wh" },
  { key: "read_misses", label: "read miss", cls: "seg-rm" },
  { key: "write_misses", label: "write miss", cls: "seg-wm" },
] as const;

export function HitMissBar({ totals }: { totals: Totals }) {
  const total = totals.hits + totals.misses;
  if (total === 0) return <p className="muted">Нет обращений к кэшу</p>;

  return (
    <div>
      <div className="bar">
        {SEGMENTS.map((s) => (
          <div
            key={s.key}
            className={s.cls}
            style={{ width: `${(100 * totals[s.key]) / total}%` }}
            title={`${s.label}: ${totals[s.key].toLocaleString("ru-RU")}`}
          />
        ))}
      </div>
      <div className="legend">
        {SEGMENTS.map((s) => (
          <span key={s.key}>
            <i className={s.cls} /> {s.label}
          </span>
        ))}
      </div>
    </div>
  );
}
