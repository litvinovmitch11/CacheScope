import { useEffect, useState } from "react";

interface FsEntry {
    name: string;
    path: string;
    kind: "dir" | "file";
}

interface FsResponse {
    path: string;
    parent: string | null;
    entries: FsEntry[];
}

interface Props {
    initialPath?: string;
    onSelect: (absolutePath: string) => void;
    onClose: () => void;
}

function dirOf(value: string): string {
    if (!value) return "";
    if (value.endsWith("/")) return value.slice(0, -1) || "/";
    const idx = value.lastIndexOf("/");
    if (idx < 0) return "";
    return idx === 0 ? "/" : value.slice(0, idx);
}

export function TracePicker({ initialPath, onSelect, onClose }: Props) {
    const [current, setCurrent] = useState("");
    const [data, setData] = useState<FsResponse | null>(null);
    const [manualPath, setManualPath] = useState(initialPath ?? "");
    const [error, setError] = useState<string | null>(null);
    const [busy, setBusy] = useState(false);

    async function load(target: string) {
        setBusy(true);
        setError(null);
        try {
            const url = new URL("/__stepper/fs", window.location.origin);
            if (target) url.searchParams.set("path", target);
            url.searchParams.set("files", "trace");
            const response = await fetch(url);
            const body = (await response.json()) as FsResponse & { error?: string };
            if (!response.ok) throw new Error(body.error ?? `HTTP ${response.status}`);
            setData(body);
            setCurrent(body.path);
        } catch (cause) {
            setError(cause instanceof Error ? cause.message : String(cause));
        } finally {
            setBusy(false);
        }
    }

    useEffect(() => {
        void load(dirOf(initialPath ?? ""));
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    return (
        <div className="stepper-modal-backdrop" onClick={onClose}>
            <div className="stepper-modal" onClick={(event) => event.stopPropagation()}>
                <header>
                    <h2>Выбор trace-файла</h2>
                    <button className="stepper-button secondary" onClick={onClose}>×</button>
                </header>

                <div className="stepper-modal-path">
                    <button
                        className="stepper-button secondary"
                        disabled={!data?.parent || busy}
                        onClick={() => data?.parent && void load(data.parent)}
                    >
                        ← Вверх
                    </button>
                    <button
                        className="stepper-button secondary"
                        disabled={busy}
                        onClick={() => void load("")}
                    >
                        ⌂ Домой
                    </button>
                    <input
                        className="stepper-modal-cwd"
                        value={current}
                        onChange={(event) => setCurrent(event.target.value)}
                        onKeyDown={(event) => {
                            if (event.key === "Enter") void load(current);
                        }}
                        spellCheck={false}
                    />
                    <button
                        className="stepper-button secondary"
                        disabled={busy}
                        onClick={() => void load(current)}
                    >
                        Открыть
                    </button>
                </div>

                <div className="stepper-modal-manual">
                    <input
                        value={manualPath}
                        onChange={(event) => setManualPath(event.target.value)}
                        placeholder="/abs/path/to/trace.json"
                        spellCheck={false}
                    />
                    <button
                        className="stepper-button primary"
                        disabled={!manualPath.trim()}
                        onClick={() => onSelect(manualPath.trim())}
                    >
                        Использовать путь
                    </button>
                </div>

                {error && <p className="stepper-error">{error}</p>}
                {busy && <p className="stepper-muted">Загрузка…</p>}

                {data && (
                    <ul className="stepper-modal-list">
                        {data.entries.map((entry) => (
                            <li key={entry.path}>
                                <button
                                    className={`stepper-modal-entry ${entry.kind}`}
                                    onClick={() =>
                                        entry.kind === "dir" ? void load(entry.path) : onSelect(entry.path)
                                    }
                                >
                                    <span className="stepper-modal-icon">
                                        {entry.kind === "dir" ? "📁" : "📄"}
                                    </span>
                                    <span className="stepper-modal-name">{entry.name}</span>
                                </button>
                            </li>
                        ))}
                        {data.entries.length === 0 && (
                            <li className="stepper-muted">Нет подходящих файлов или папок.</li>
                        )}
                    </ul>
                )}
            </div>
        </div>
    );
}
