import type { SetStat } from "../hooks/useStepperSession";

interface Props {
    setCount: number;
    stats: Record<number, SetStat>;
    levelName: string;
}

function colorFor(stat: SetStat | undefined, maxTotal: number): string {
    if (!stat) return "hsl(0, 0%, 88%)";
    const total = stat.hits + stat.misses;
    if (total === 0) return "hsl(0, 0%, 88%)";
    const hitRate = stat.hits / total;
    const hue = Math.round(hitRate * 120);
    const light = 80 - 25 * (total / Math.max(1, maxTotal));
    return `hsl(${hue}, 70%, ${light}%)`;
}

export function HeatmapGrid({ setCount, stats, levelName }: Props) {
    if (setCount === 0) {
        return <p className="stepper-muted">Нет сетов для отображения.</p>;
    }

    const sets = Array.from({ length: setCount }, (_, i) => i);
    const maxTotal = Math.max(
        1,
        ...sets.map((s) => {
            const v = stats[s];
            return v ? v.hits + v.misses : 0;
        }),
    );

    return (
        <div className="stepper-heatmap">
            <div className="stepper-heatmap-legend">
                <span>misses</span>
                <span className="stepper-heatmap-gradient" />
                <span>hits</span>
                <span className="stepper-muted" style={{ marginLeft: "auto" }}>
                    {levelName} · {setCount} sets
                </span>
            </div>
            <div className="stepper-heatmap-cells">
                {sets.map((s) => {
                    const v = stats[s];
                    const total = v ? v.hits + v.misses : 0;
                    const title = v
                        ? `Set ${s}: ${v.hits} hits, ${v.misses} misses (${total} accesses)`
                        : `Set ${s}: нет обращений`;
                    return (
                        <div
                            key={s}
                            className="stepper-heatmap-cell"
                            title={title}
                            style={{ background: colorFor(v, maxTotal) }}
                        >
                            {s}
                        </div>
                    );
                })}
            </div>
        </div>
    );
}
