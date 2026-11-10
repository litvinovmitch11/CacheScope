import type { LevelView } from "../metrics";

interface Props {
    levels: LevelView[];
    value: number;
    onChange: (level: number) => void;
}

/** Переключатель уровня. Для одного уровня не показывается. */
export function LevelTabs({ levels, value, onChange }: Props) {
    if (levels.length < 2) return null;
    return (
        <div className="tabs">
            {levels.map((l) => (
                <button
                    key={l.level}
                    type="button"
                    className={l.level === value ? "tab active" : "tab"}
                    onClick={() => onChange(l.level)}
                >
                    {l.name}
                </button>
            ))}
        </div>
    );
}
