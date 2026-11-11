import { RunnerClient } from "../api/runnerClient";
import { readStageText } from "../api/localFiles";
import { useEffect, useMemo, useState } from "react";
import type { RunnerLogEntry } from "../api/runnerClient";
import { useStepperSession } from "../hooks/useStepperSession";
import type { EventOutcome, LevelConfig, SourceStage } from "../types/runner";
import { TracePicker } from "./TracePicker";
import { HeatmapGrid } from "./HeatmapGrid";
import { HitMissTimeline } from "./HitMissTimeline";
import { plural } from "../../../plural";

const DEFAULT_LEVEL: LevelConfig = {
  name: "L1", size_bytes: 32768, line_size_bytes: 64, associativity: 8, replacement_policy: "LRU",
};
function isPowerOfTwo(value: number) {
  return Number.isSafeInteger(value) && value > 0 && Number.isInteger(Math.log2(value));
}

function numberValue(value: string, fallback: number) {
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed > 0 ? parsed : fallback;
}

type HighlightKind = "hit" | "miss";

function buildHighlights(
  event: EventOutcome | null,
): Map<number, Map<string, HighlightKind>> {
  const result = new Map<number, Map<string, HighlightKind>>();
  if (!event) return result;
  for (const lr of event.level_results) {
    const perLevel = new Map<string, HighlightKind>();
    for (const line of lr.lines) {
      if (line.way === null) continue;
      perLevel.set(`${line.set}-${line.way}`, line.hit ? "hit" : "miss");
    }
    result.set(lr.level, perLevel);
  }
  return result;
}

export function StepperWorkspace() {
  const [baseUrl, setBaseUrl] = useState("http://127.0.0.1:9055/api/v1");
  const [tracePath, setTracePath] = useState("");
  const [skipCount, setSkipCount] = useState(1);
  const [levels, setLevels] = useState<LevelConfig[]>([{ ...DEFAULT_LEVEL }]);
  const [requestLogs, setRequestLogs] = useState<RunnerLogEntry[]>([]);
  const [pickerOpen, setPickerOpen] = useState(false);
  const client = useMemo(
    () =>
      new RunnerClient(baseUrl, (entry) => {
        if (!entry.ok) {
          setRequestLogs((previous) => [entry, ...previous].slice(0, 100));
        }
      }),
    [baseUrl],
  );
  const [recentTraces, setRecentTraces] = useState<string[]>([]);

  useEffect(() => {
    void (async () => {
      try {
        const response = await client.listSessions();
        const unique = Array.from(
          new Set(
            response.sessions
              .map((item) => item.trace_path)
              .filter((value): value is string => typeof value === "string" && value.length > 0),
          ),
        );
        setRecentTraces(unique.slice(0, 5));
      } catch {
        // молча — не критично для работы UI
      }
    })();
  }, [client]);
  const runner = useStepperSession(client);
  const [selectedLevel, setSelectedLevel] = useState(1);
  const [selectedStage, setSelectedStage] = useState("");
  const [stageText, setStageText] = useState("");
  const [stageError, setStageError] = useState<string | null>(null);
  const [stageBusy, setStageBusy] = useState(false);
  const stages = runner.metadata?.source.source_map?.stages ?? [];
  const currentEvent = runner.progress?.last_event ?? null;
  const highlights = useMemo(() => buildHighlights(currentEvent), [currentEvent]);
  const currentAccessId = currentEvent?.trace_event.access_id;
  const currentAccess = runner.metadata?.accesses.find((item) => item.access_id === currentAccessId);
  const locations = currentAccess?.stage_locations ?? [];
  const currentStageLocation = locations.find((location) => location.stage_id === selectedStage) ?? locations[0];
  const activeStage = stages.find((stage) => stage.stage_id === selectedStage) ?? stages.find((stage) => stage.stage_id === currentStageLocation?.stage_id);
  const selectedSnapshot = runner.cachePages.find((item) => item.level === selectedLevel);
  const allSets = selectedSnapshot?.pages.flatMap((page) => page.sets) ?? [];

  async function loadStage(stage: SourceStage) {
    if (!runner.session) return;
    setSelectedStage(stage.stage_id);
    setStageBusy(true); setStageError(null);
    try {
      setStageText(await readStageText(runner.session.trace_path, stage.path));
    } catch (error) {
      setStageText("");
      setStageError(error instanceof Error ? error.message : "Не удалось прочитать MLIR-файл");
    } finally { setStageBusy(false); }
  }

  function updateLevel(index: number, patch: Partial<LevelConfig>) {
    setLevels((current) => current.map((level, i) => i === index ? { ...level, ...patch } : level));
  }

  const canCreate = tracePath.startsWith("/") && levels.length > 0 && levels.every((level) =>
    level.name.trim() && level.size_bytes > 0 && level.line_size_bytes > 0 && level.associativity > 0 &&
    level.size_bytes % (level.line_size_bytes * level.associativity) === 0 &&
    Number.isInteger(level.size_bytes / (level.line_size_bytes * level.associativity)) &&
    isPowerOfTwo(level.line_size_bytes) && isPowerOfTwo(level.size_bytes / (level.line_size_bytes * level.associativity)),
  ) && levels.every((level) => level.line_size_bytes === levels[0].line_size_bytes);

  return (
    <main className="stepper-shell">
      <div className="stepper-topbar">
        <div>
          <p className="stepper-eyebrow">CACHESCOPE / INTERACTIVE</p>
          <h1>Пошаговая симуляция</h1>
          <p className="stepper-subtitle">Выполнение trace по одному событию с просмотром физического состояния кэша.</p>
        </div>
        <a className="stepper-back" href="/">← К статистике</a>
      </div>

      {!runner.session && (
        <section className="stepper-panel">
          <h2>Новая сессия</h2>
          <label className="stepper-field">
            <span>Адрес backend API</span>
            <input value={baseUrl} onChange={(event) => setBaseUrl(event.target.value)} spellCheck={false} />
          </label>
          <label className="stepper-field">
            <span>Абсолютный путь к trace-файлу</span>
            <div style={{ display: "flex", gap: 8 }}>
              <input
                value={tracePath}
                onChange={(event) => setTracePath(event.target.value)}
                placeholder="/home/user/traces/trace.json"
                spellCheck={false}
                style={{ flex: 1 }}
              />
              <button
                type="button"
                className="stepper-button secondary"
                onClick={() => setPickerOpen(true)}
              >
                Обзор…
              </button>
            </div>
            <small>Файл не загружается через браузер. Backend открывает его непосредственно по этому пути.</small>
          </label>
          {recentTraces.length > 0 && (
            <div className="stepper-recent-traces">
              <span className="stepper-muted">Недавние:</span>
              {recentTraces.map((value) => (
                <button
                  key={value}
                  type="button"
                  className="stepper-chip"
                  onClick={() => setTracePath(value)}
                  title={value}
                >
                  {value.split("/").slice(-2).join("/")}
                </button>
              ))}
            </div>
          )}
          <h3>Конфигурация кэша</h3>
          {levels.map((level, index) => (
            <div className="stepper-level-config" key={index}>
              <label><span>Имя</span><input value={level.name} onChange={(e) => updateLevel(index, { name: e.target.value })} /></label>
              <label><span>Размер, байт</span><input type="number" min="1" value={level.size_bytes} onChange={(e) => updateLevel(index, { size_bytes: numberValue(e.target.value, level.size_bytes) })} /></label>
              <label><span>Размер линии, байт</span><input type="number" min="1" value={level.line_size_bytes} onChange={(e) => updateLevel(index, { line_size_bytes: numberValue(e.target.value, level.line_size_bytes) })} /></label>
              <label><span>Associativity</span><input type="number" min="1" value={level.associativity} onChange={(e) => updateLevel(index, { associativity: numberValue(e.target.value, level.associativity) })} /></label>
              <label><span>Policy</span><select value={level.replacement_policy} onChange={(e) => updateLevel(index, { replacement_policy: e.target.value as LevelConfig["replacement_policy"] })}><option>LRU</option><option>FIFO</option><option>Random</option></select></label>
              {level.replacement_policy === "Random" && <label><span>Seed</span><input value={level.random_seed ?? "0"} onChange={(e) => updateLevel(index, { random_seed: e.target.value })} /></label>}
              {levels.length > 1 && <button className="stepper-remove" onClick={() => setLevels((items) => items.filter((_, i) => i !== index))}>Удалить</button>}
            </div>
          ))}
          <div className="stepper-actions">
            <button className="stepper-button secondary" onClick={() => setLevels((items) => [...items, { ...DEFAULT_LEVEL, name: `L${items.length + 1}` }])}>Добавить уровень</button>
            <button className="stepper-button primary" disabled={!canCreate || runner.busy} onClick={() => void runner.create(tracePath, levels)}>Создать сессию</button>
          </div>
          <p className="stepper-note">Все уровни должны иметь одинаковый размер cache line. Число sets вычисляется backend.</p>
        </section>
      )}

      {runner.error && <div className="stepper-error" role="alert">{runner.error}</div>}
      {runner.busy && <div className="stepper-loading">Выполняется запрос к runner…</div>}

      {runner.session && (
        <>
          <section className="stepper-sessionbar">
            <div><span className="stepper-muted">Сессия</span><strong>{runner.session.session_id}</strong></div>
            <div><span className="stepper-muted">Статус</span><strong>{runner.session.status}</strong></div>
            <div><span className="stepper-muted">Событие</span><strong>{runner.progress?.cursor ?? runner.session.cursor} / {runner.session.total_events ?? "…"}</strong></div>
            <div><span className="stepper-muted">Ревизия</span><strong>{runner.progress?.revision ?? runner.session.revision}</strong></div>
            <button className="stepper-button secondary" disabled={runner.busy} onClick={() => void runner.clear()}>Закрыть сессию</button>
          </section>

          {runner.session.status === "failed" && <div className="stepper-error">{runner.session.error?.message ?? "Сессия завершилась ошибкой"}</div>}

          <section className="stepper-controls">
            <button className="stepper-button primary stepper-next" disabled={runner.busy || !runner.progress || runner.progress.status === "finished"} onClick={() => void runner.step()}>Следующий шаг →</button>

            <label className="stepper-field" style={{ display: "inline-grid", gap: 4, margin: 0 }}>
              <span>Пропустить шагов</span>
              <input
                type="number"
                min={1}
                max={runner.session.limits.max_step_count}
                value={skipCount}
                onChange={(e) => setSkipCount(Number(e.target.value))}
                disabled={runner.busy || !runner.progress || runner.progress.status === "finished"}
                style={{ width: 110 }}
              />
            </label>

            <button
              className="stepper-button secondary"
              disabled={
                runner.busy ||
                !runner.progress ||
                runner.progress.status === "finished" ||
                skipCount < 1 ||
                skipCount > runner.session.limits.max_step_count
              }
              onClick={() => void runner.step(skipCount)}
            >
              Пропустить {skipCount} →
            </button>

            <button className="stepper-button secondary" disabled={runner.busy || !runner.progress} onClick={() => void runner.reset()}>Сбросить</button>

            <span className="stepper-note">
              «Следующий шаг» выполняет 1 событие. «Пропустить» выполняет введённое число событий одним batch-запросом.
            </span>
          </section>

          <div className="stepper-grid">
            <section className="stepper-panel">
              <div className="stepper-panel-heading"><h2>Текущее событие</h2><span className="stepper-tag">{currentEvent ? `#${currentEvent.trace_event.sequence}` : "нет события"}</span></div>
              {currentEvent ? (
                <>
                  <dl className="stepper-kv">
                    <dt>Тип</dt><dd>{currentEvent.trace_event.kind}</dd>
                    <dt>Access ID</dt><dd>{currentEvent.trace_event.access_id}</dd>
                    <dt>Memref ID</dt><dd>{currentEvent.trace_event.memref_id}</dd>
                    <dt>Byte offset</dt><dd>{currentEvent.trace_event.byte_offset}</dd>
                    <dt>Размер</dt><dd>{currentEvent.trace_event.size_bytes} B</dd>
                    <dt>Адрес</dt><dd>{currentEvent.address}</dd>
                  </dl>
                  <h3>Результаты по уровням</h3>
                  {currentEvent.level_results.map((result) => (
                    <div className="stepper-event-level" key={result.level}><strong>L{result.level}</strong>{result.lines.map((line, index) => <span key={index} className={line.hit ? "hit" : "miss"}>{line.hit ? "HIT" : "MISS"} · set {line.set} · way {line.way ?? "—"}</span>)}</div>
                  ))}
                  {currentEvent.evictions.length > 0 && <><h3>Вытеснения</h3>{currentEvent.evictions.map((item, index) => <p className="stepper-small" key={index}>L{item.level} · {item.line_address} · {item.dirty ? "dirty" : "clean"}</p>)}</>}
                </>
              ) : <p className="stepper-muted">Выполните первый шаг, чтобы увидеть событие.</p>}
            </section>

            <section className="stepper-panel">
              <div className="stepper-panel-heading"><h2>MLIR source</h2><span className="stepper-tag">{currentAccess ? `access ${currentAccess.access_id}` : "source map"}</span></div>
              {stages.length > 0 ? (
                <>
                  <label className="stepper-field"><span>Stage</span><select value={activeStage?.stage_id ?? selectedStage} onChange={(e) => { const stage = stages.find((item) => item.stage_id === e.target.value); if (stage) void loadStage(stage); }}><option value="">Выберите stage</option>{stages.map((stage) => <option key={stage.stage_id} value={stage.stage_id}>{stage.stage_id} — {stage.path}</option>)}</select></label>
                  {activeStage && !stageText && !stageBusy && <button className="stepper-button secondary" onClick={() => void loadStage(activeStage)}>Загрузить MLIR</button>}
                  {stageBusy && <p className="stepper-muted">Чтение файла…</p>}
                  {stageError && <p className="stepper-error">{stageError}</p>}
                  {currentStageLocation && <p className="stepper-note">Позиция: строка {currentStageLocation.line}, колонка {currentStageLocation.column}; {currentStageLocation.operation}</p>}
                  {stageText && <pre className="stepper-source">{stageText.split("\n").map((line, index) => <div className={currentStageLocation?.line === index + 1 ? "source-line active" : "source-line"} key={index}><span>{index + 1}</span><code>{line || " "}</code></div>)}</pre>}
                </>
              ) : <p className="stepper-muted">В metadata нет source_map или сохранённых stage-файлов.</p>}
              {currentAccess && <p className="stepper-note">{currentAccess.source_location}</p>}
            </section>
          </div>

          <section className="stepper-panel">
            <div className="stepper-panel-heading"><h2>Физическое состояние кэша</h2><div className="stepper-tabs">{runner.session.levels.map((level) => <button key={level.level} className={selectedLevel === level.level ? "active" : ""} onClick={() => setSelectedLevel(level.level)}>{level.name}</button>)}</div></div>
            <div className="stepper-cache-meta">{runner.session.levels.find((level) => level.level === selectedLevel)?.set_count ?? 0} sets · {runner.session.levels.find((level) => level.level === selectedLevel)?.associativity ?? 0} ways</div>
            <div className="stepper-cache-grid">
              {
                allSets.map((set) => (
                  <div className="stepper-set" key={set.set}>
                    <strong>Set {set.set}</strong>
                    <div className="stepper-ways">
                      {set.ways.map((way) => {
                        const kind = highlights.get(selectedLevel)?.get(`${set.set}-${way.way}`);
                        const cls =
                          "stepper-way" +
                          (way.valid ? " valid" : "") +
                          (kind === "hit" ? " way-hit" : "") +
                          (kind === "miss" ? " way-miss" : "");
                        return (
                          <div className={cls} key={way.way}>
                            <b>Way {way.way}</b>
                            <span>{way.valid ? way.line_address : "INVALID"}</span>
                            {way.valid && <small>{way.dirty ? "DIRTY" : "CLEAN"}</small>}
                          </div>
                        );
                      })}
                    </div>
                  </div>
                ))}
            </div>
          </section>
          <section className="stepper-panel">
            <div className="stepper-panel-heading">
              <h2>Тепловая карта сетов</h2>
              <span className="stepper-tag">{runner.session.levels.length} {plural(runner.session.levels.length, "уровень", "уровня", "уровней")}</span>
            </div>
            <p className="stepper-note">
              Hit rate по каждому сету для всех уровней кэша, накопленный по всем выполненным событиям.
              Наведите на ячейку, чтобы увидеть точные значения.
            </p>
            <div className="stepper-heatmap-stack">
              {runner.session.levels.map((level) => (
                <div key={level.level} className="stepper-heatmap-block">
                  <h3 className="stepper-heatmap-title">
                    {level.name}{" "}
                    <span className="stepper-muted">
                      {level.set_count} sets · {level.associativity}-way ·{" "}
                      {level.replacement_policy}
                    </span>
                  </h3>
                  <HeatmapGrid
                    setCount={level.set_count}
                    levelName={level.name}
                    stats={runner.setStats[level.level] ?? {}}
                  />
                </div>
              ))}
            </div>
            <section className="stepper-panel">
              <div className="stepper-panel-heading">
                <h2>Hit / miss по шагам</h2>
                <span className="stepper-tag">все уровни</span>
              </div>
              <p className="stepper-note">
                Одна ячейка — один шаг. Зелёный — попадания, красный — промахи; если событие
                затронуло несколько cache line, ячейка делится по доле попаданий и промахов.
                Серые ячейки — шаги без обращений к этому уровню.
              </p>
              <div className="stepper-timeline-stack">
                {runner.session.levels.map((level) => (
                  <HitMissTimeline
                    key={level.level}
                    levelName={level.name}
                    points={runner.stepHistory[level.level] ?? []}
                  />
                ))}
              </div>
            </section>
          </section>
          {runner.progress && <section className="stepper-panel"><h2>Накопительная статистика</h2><div className="stepper-stats"><div><span>Hits</span><strong>{runner.progress.summary.totals.hits}</strong></div><div><span>Misses</span><strong>{runner.progress.summary.totals.misses}</strong></div><div><span>Evictions</span><strong>{runner.progress.summary.totals.evictions}</strong></div><div><span>Writebacks</span><strong>{runner.progress.summary.totals.writebacks}</strong></div><div><span>Hit rate</span><strong>{runner.progress.summary.totals.hit_rate_percent == null ? "—" : `${runner.progress.summary.totals.hit_rate_percent.toFixed(2)}%`}</strong></div></div></section>}
        </>
      )}
      <section className="stepper-panel stepper-logs">
        <div className="stepper-panel-heading">
          <h2>Журнал запросов к backend</h2>

          <div className="stepper-actions">
            <span className="stepper-tag">
              {requestLogs.length} {plural(requestLogs.length, "запись", "записи", "записей")}
            </span>

            <button
              className="stepper-button secondary"
              onClick={() => {
                const text = requestLogs.map((log) => [
                  `[${log.time}]`,
                  `${log.method} ${log.url}`,
                  `Status: ${log.status ?? "NETWORK ERROR"}`,
                  `Duration: ${log.duration} ms`,
                  `Success: ${log.ok}`,
                  log.response,
                ].join("\n")).join("\n\n-------------------------\n\n");

                void navigator.clipboard.writeText(text);
              }}
              disabled={requestLogs.length === 0}
            >
              Копировать лог
            </button>

            <button
              className="stepper-button secondary"
              onClick={() => setRequestLogs([])}
            >
              Очистить
            </button>
          </div>
        </div>

        {requestLogs.length === 0 ? (
          <p className="stepper-muted">
            Запросов пока нет. Ошибки появятся здесь автоматически.
          </p>
        ) : (
          <div className="stepper-log-list">
            {[...requestLogs].reverse().map((log) => (
              <details
                className={`stepper-log-entry ${log.ok ? "success" : "failure"}`}
                key={log.id}
                open={!log.ok}
              >
                <summary>
                  <span className="stepper-log-time">{log.time}</span>

                  <strong>{log.method}</strong>

                  <span className="stepper-log-url">
                    {log.url}
                  </span>

                  <span className={`stepper-log-status ${log.ok ? "ok" : "bad"}`}>
                    {log.status ?? "NETWORK ERROR"}
                  </span>

                  <span className="stepper-log-duration">
                    {log.duration} ms
                  </span>
                </summary>

                <div className="stepper-log-details">
                  <p>
                    <strong>Результат:</strong> {log.ok ? "Успешно" : "Ошибка"}
                  </p>

                  {!log.ok && (
                    <>
                      <strong>Ответ сервера / причина ошибки:</strong>
                      <pre>{log.response}</pre>
                    </>
                  )}
                </div>
              </details>
            ))}
          </div>
        )}
      </section>
      {pickerOpen && (
        <TracePicker
          initialPath={tracePath || undefined}
          onSelect={(value) => { setTracePath(value); setPickerOpen(false); }}
          onClose={() => setPickerOpen(false)}
        />
      )}
    </main>
  );
}
