import type { StepPoint } from "../hooks/useStepperSession";
import { plural } from "../../../plural";

const MAX_TIMELINE_POINTS = 2000;

interface Props {
    levelName: string;
    points: StepPoint[];
}

function cellStyle(p: StepPoint): React.CSSProperties {
    if (p.neutral) return {};
    const total = p.hits + p.misses;
    if (total === 0) return {};
    if (p.hits === total) return { background: "#16a34a" };
    if (p.misses === total) return { background: "#dc2626" };
    const hitPct = (p.hits / total) * 100;
    return {
        background: `linear-gradient(to top, #16a34a ${hitPct}%, #dc2626 ${hitPct}%)`,
    };
}

function cellTitle(p: StepPoint, levelName: string): string {
    if (p.neutral) return `шаг ${p.sequence}: нет обращения к ${levelName}`;
    const total = p.hits + p.misses;
    return `шаг ${p.sequence}: ${p.hits} hit / ${p.misses} miss (${total} ${plural(total, "линия", "линии", "линий")})`;
}

export function HitMissTimeline({ levelName, points }: Props) {
    if (points.length === 0) {
        return (
            <div className="stepper-timeline">
                <div className="stepper-timeline-head">
                    <strong>{levelName}</strong>
                    <span className="stepper-muted">нет данных</span>
                </div>
            </div>
        );
    }

    const totalCount = points.length;
    const visible =
        totalCount > MAX_TIMELINE_POINTS
            ? points.slice(-MAX_TIMELINE_POINTS)
            : points;

    const totals = visible.reduce(
        (acc, p) =>
            p.neutral ? acc : { h: acc.h + p.hits, m: acc.m + p.misses },
        { h: 0, m: 0 },
    );
    const activeSteps = visible.filter((p) => !p.neutral).length;

    return (
        <div className="stepper-timeline">
            <div className="stepper-timeline-head">
                <strong>{levelName}</strong>
                <span className="stepper-muted">
                    {totalCount > MAX_TIMELINE_POINTS
                        ? `показаны последние ${visible.length} шагов (всего ${totalCount})`
                        : `${totalCount} ${plural(totalCount, "шаг", "шага", "шагов")}`}{" "}
                    · {activeSteps} с обращениями · {totals.h} hit / {totals.m} miss
                </span>
            </div>
            <div className="stepper-timeline-cells">
                {visible.map((p) => {
                    const cls =
                        "stepper-timeline-cell" + (p.neutral ? " neutral" : "");
                    return (
                        <div
                            key={p.sequence}
                            className={cls}
                            style={cellStyle(p)}
                            title={cellTitle(p, levelName)}
                        />
                    );
                })}
            </div>
            <div className="stepper-timeline-legend">
                <span>
                    <i style={{ background: "#16a34a" }} /> hit
                </span>
                <span>
                    <i style={{ background: "#dc2626" }} /> miss
                </span>
                <span>
                    <i
                        className="stepper-timeline-legend-mixed"
                        style={{
                            background:
                                "linear-gradient(to top, #16a34a 50%, #dc2626 50%)",
                        }}
                    />{" "}
                    смешанное
                </span>
                <span>
                    <i
                        style={{
                            background: "var(--line)",
                            opacity: 0.5,
                        }}
                    />{" "}
                    нет обращения
                </span>
            </div>
        </div>
    );
}
