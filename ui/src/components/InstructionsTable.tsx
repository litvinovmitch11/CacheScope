import type { Instruction } from "../types";
import { fmtCount, fmtPct, ratio } from "../metrics";

interface Props {
    instructions: Instruction[] | undefined;
    level: number;
}

export function InstructionsTable({ instructions, level }: Props) {
    if (!instructions) {
        return (
            <p className="muted">
                Поинструкционная статистика недоступна: <code>instructions</code> отсутствует.
            </p>
        );
    }

    return (
        <details>
            <summary>Инструкции ({instructions.length})</summary>
            <div className="instructions-table-wrap">
                <table className="cmp small instructions-table">
                    <thead>
                        <tr>
                            <th>access_id</th>
                            <th>memref</th>
                            <th>kind</th>
                            <th>events</th>
                            <th>req. bytes</th>
                            <th>line acc.</th>
                            <th>hit rate</th>
                            <th>misses</th>
                            <th>evictions</th>
                            <th>writebacks</th>
                            <th>back inv.</th>
                        </tr>
                    </thead>
                    <tbody>
                        {instructions.map((ins) => {
                            const L = ins.levels.find((l) => l.level === level);
                            const n = L?.processed.line_accesses;
                            return (
                                <tr key={ins.access_id}>
                                    <td>{ins.access_id}</td>
                                    <td>{ins.memref_id}</td>
                                    <td>{ins.kind}</td>
                                    <td>{fmtCount(ins.events)}</td>
                                    <td>{fmtCount(ins.requested_bytes)}</td>
                                    <td>{fmtCount(n)}</td>
                                    <td>{L && n !== undefined ? fmtPct(ratio(L.totals.hits, n)) : "—"}</td>
                                    <td>{fmtCount(L?.totals.misses)}</td>
                                    <td>{fmtCount(L?.totals.evictions)}</td>
                                    <td>{fmtCount(L?.totals.writebacks)}</td>
                                    <td>{fmtCount(L?.totals.back_invalidations)}</td>
                                </tr>
                            );
                        })}
                    </tbody>
                </table>
            </div>
            <p className="muted">
                Показан выбранный уровень. «req. bytes» — сумма size_bytes событий инструкции, это не
                трафик между кэшами. Строки без обращений на уровне дают «—», а не нулевой hit rate.
            </p>
        </details>
    );
}
