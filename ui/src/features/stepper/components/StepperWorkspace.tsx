import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { RunnerClient } from "../api/runnerClient";
import type { RunnerLogEntry } from "../api/runnerClient";
import { useStepperSession } from "../hooks/useStepperSession";
import type { EventOutcome, HierarchyConfig, LevelConfig, SourceStage } from "../types/runner";
import { HeatmapGrid } from "./HeatmapGrid";
import { HitMissTimeline } from "./HitMissTimeline";
import { plural } from "../../../plural";
import { CACHE_PRESETS } from "../../../cachePresets";
import { ProductNav } from "../../../components/ProductNav";

const DEFAULT_LEVEL: LevelConfig = {
  name: "L1",
  size_bytes: 32768,
  line_size_bytes: 64,
  associativity: 8,
  replacement_policy: "LRU",
};

function isPowerOfTwo(value: number) {
  return (
    Number.isSafeInteger(value) &&
    value > 0 &&
    Number.isInteger(Math.log2(value))
  );
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

type CacheLevelSnapshot = Readonly<{
  size_bytes: number;
  line_size_bytes: number;
  associativity: number;
  replacement_policy: string;
  write_policy?: string;
  allocation_policy?: string;
}>;

function presetToLevelPatch(preset: {
  levels?: readonly CacheLevelSnapshot[];
}): Partial<LevelConfig> | null {
  const lvl = preset.levels?.[0];
  if (!lvl) return null;
  return {
    size_bytes: lvl.size_bytes,
    line_size_bytes: lvl.line_size_bytes,
    associativity: lvl.associativity,
    replacement_policy: lvl.replacement_policy as LevelConfig["replacement_policy"],
    write_policy: lvl.write_policy as LevelConfig["write_policy"],
    allocation_policy: lvl.allocation_policy as LevelConfig["allocation_policy"],
  };
}

type PresetComparableLevel = Pick<
  LevelConfig,
  | "size_bytes"
  | "line_size_bytes"
  | "associativity"
  | "replacement_policy"
  | "write_policy"
  | "allocation_policy"
>;

function findMatchingPresetId(level: PresetComparableLevel): string | null {
  for (const preset of CACHE_PRESETS) {
    if (preset.levels.length !== 1) continue;
    const lvl = preset.levels?.[0];
    if (!lvl) continue;
    if (
      lvl.size_bytes === level.size_bytes &&
      lvl.line_size_bytes === level.line_size_bytes &&
      lvl.associativity === level.associativity &&
      lvl.replacement_policy === level.replacement_policy &&
      (lvl.write_policy ?? "write_back") === (level.write_policy ?? "write_back") &&
      (lvl.allocation_policy ?? "write_allocate") === (level.allocation_policy ?? "write_allocate")
    ) {
      return preset.id;
    }
  }
  return null;
}

export function StepperWorkspace() {
  const [baseUrl, setBaseUrl] = useState(() => `${window.location.origin}/api/v1`);
  const query = new URLSearchParams(window.location.search);
  const experimentId = query.get("experiment");
  const experimentVariant = query.get("variant");
  const experimentProfile = query.get("profile");
  const autoStartArtifact = query.get("autostart") === "1";
  const artifactAttempt = useRef("");

  // Загрузка файлов на runner
  const [uploadedTracePath, setUploadedTracePath] = useState<string | null>(null);
  const [uploadedStageCount, setUploadedStageCount] = useState(0);
  const [uploadedFileName, setUploadedFileName] = useState<string | null>(null);
  const [uploadBusy, setUploadBusy] = useState(false);
  const [uploadError, setUploadError] = useState<string | null>(null);

  const [skipCount, setSkipCount] = useState(1);
  const [levels, setLevels] = useState<LevelConfig[]>([{ ...DEFAULT_LEVEL }]);
  const [hierarchy, setHierarchy] = useState<HierarchyConfig | undefined>();
  const [requestLogs, setRequestLogs] = useState<RunnerLogEntry[]>([]);
  const [artifactSelection, setArtifactSelection] = useState<{
    experimentId: string;
    variant: string;
    profile: string | null;
  } | null>(null);

  const client = useMemo(
    () =>
      new RunnerClient(baseUrl, (entry) => {
        if (!entry.ok) {
          setRequestLogs((previous) => [entry, ...previous].slice(0, 100));
        }
      }),
    [baseUrl],
  );

  const runner = useStepperSession(client);
  const createStepperSession = runner.create;
  const [selectedLevel, setSelectedLevel] = useState(1);
  const [selectedStage, setSelectedStage] = useState("");
  const [stageText, setStageText] = useState("");
  const [stageError, setStageError] = useState<string | null>(null);
  const [stageBusy, setStageBusy] = useState(false);

  const stages = runner.metadata?.source.source_map?.stages ?? [];
  const currentEvent = runner.progress?.last_event ?? null;
  const highlights = useMemo(
    () => buildHighlights(currentEvent),
    [currentEvent],
  );
  const currentAccessId = currentEvent?.trace_event.access_id;
  const currentAccess = runner.metadata?.accesses.find(
    (item) => item.access_id === currentAccessId,
  );
  const locations = currentAccess?.stage_locations ?? [];
  const currentStageLocation =
    locations.find((location) => location.stage_id === selectedStage) ??
    locations[0];
  const activeStage =
    stages.find((stage) => stage.stage_id === selectedStage) ??
    stages.find((stage) => stage.stage_id === currentStageLocation?.stage_id);
  const selectedSnapshot = runner.cachePages.find(
    (item) => item.level === selectedLevel,
  );
  const allSets = selectedSnapshot?.pages.flatMap((page) => page.sets) ?? [];

  const onFilesPicked = useCallback(async (files: File[]) => {
    if (files.length === 0) return;
    setUploadBusy(true);
    setUploadError(null);
    setUploadedTracePath(null);
    setUploadedStageCount(0);
    setUploadedFileName(
      files.find((f) => /trace\.json$/i.test(f.name))?.name ?? null,
    );
    try {
      const result = await client.uploadArtifacts(files);
      setUploadedTracePath(result.trace_path);
      setUploadedStageCount(result.stage_paths.length);
      return result.trace_path;
    } catch (error) {
      setUploadError(
        error instanceof Error ? error.message : "Не удалось загрузить файлы",
      );
    } finally {
      setUploadBusy(false);
    }
  }, [client]);

  useEffect(() => {
    const artifactKey = `${experimentId ?? ""}:${experimentVariant ?? ""}:${experimentProfile ?? ""}`;
    if (!experimentId || artifactAttempt.current === artifactKey) return;
    artifactAttempt.current = artifactKey;
    setUploadBusy(true);
    setUploadError(null);
    client.downloadExperimentStepper(experimentId, experimentVariant, experimentProfile)
      .then(async (artifact) => {
        setArtifactSelection({
          experimentId: artifact.experimentId,
          variant: artifact.variant,
          profile: artifact.profile,
        });
        setLevels(artifact.levels);
        setHierarchy(artifact.hierarchy);
        const tracePath = await onFilesPicked(artifact.files);
        if (autoStartArtifact && tracePath) {
          await createStepperSession(tracePath, artifact.levels, artifact.hierarchy);
        }
      })
      .catch((error) => {
        setUploadError(
          error instanceof Error
            ? error.message
            : "Не удалось открыть experiment artifact",
        );
        setUploadBusy(false);
      });
  }, [autoStartArtifact, client, createStepperSession, experimentId, experimentProfile, experimentVariant, onFilesPicked]);

  async function loadStage(stage: SourceStage) {
    if (!runner.session) return;
    setSelectedStage(stage.stage_id);
    setStageBusy(true);
    setStageError(null);
    try {
      setStageText(
        await client.readSource(runner.session.trace_path, stage.path),
      );
    } catch (error) {
      setStageText("");
      setStageError(
        error instanceof Error ? error.message : "Не удалось прочитать MLIR-файл",
      );
    } finally {
      setStageBusy(false);
    }
  }

  function updateLevel(index: number, patch: Partial<LevelConfig>) {
    setLevels((current) =>
      current.map((level, i) => {
        if (i !== index) return level;
        const updated = { ...level, ...patch };
        if (updated.replacement_policy !== "Random") delete updated.random_seed;
        return updated;
      }),
    );
  }

  function resizeHierarchy(levelCount: number) {
    setHierarchy((current) => current ? {
      ...current,
      inclusion_policies: Array.from(
        { length: Math.max(0, levelCount - 1) },
        (_, index) => current.inclusion_policies[index] ?? "exclusive",
      ),
    } : undefined);
  }

  function removeLevel(index: number) {
    const next = levels.filter((_, position) => position !== index);
    setLevels(next);
    resizeHierarchy(next.length);
  }

  function addLevel() {
    const last = levels[levels.length - 1];
    const next = [
      ...levels,
      {
        ...DEFAULT_LEVEL,
        name: `L${levels.length + 1}`,
        size_bytes: last.size_bytes * 4,
        line_size_bytes: levels[0].line_size_bytes,
      },
    ];
    setLevels(next);
    resizeHierarchy(next.length);
  }

  function applyPreset(index: number, presetId: string) {
    const preset = CACHE_PRESETS.find((p) => p.id === presetId);
    if (!preset) return;
    const patch = presetToLevelPatch(preset);
    if (patch) updateLevel(index, patch);
  }

  const canCreate =
    !!uploadedTracePath &&
    levels.length > 0 &&
    levels.every(
      (level) =>
        level.name.trim() &&
        level.size_bytes > 0 &&
        level.line_size_bytes > 0 &&
        level.associativity > 0 &&
        level.size_bytes % (level.line_size_bytes * level.associativity) === 0 &&
        Number.isInteger(
          level.size_bytes / (level.line_size_bytes * level.associativity),
        ) &&
        isPowerOfTwo(level.line_size_bytes) &&
        isPowerOfTwo(
          level.size_bytes / (level.line_size_bytes * level.associativity),
        ),
    ) &&
    levels.every((level) => level.line_size_bytes === levels[0].line_size_bytes) &&
    (!hierarchy || hierarchy.inclusion_policies.length === levels.length - 1);
  const effectiveLevels = runner.session?.levels ?? levels;
  const manualPresetId = !artifactSelection && effectiveLevels.length === 1
    ? findMatchingPresetId(effectiveLevels[0])
    : null;
  const selectedPreset = artifactSelection?.profile
    ? CACHE_PRESETS.find((preset) => preset.id === artifactSelection.profile)
    : CACHE_PRESETS.find((preset) => preset.id === manualPresetId);
  const profileLabel = artifactSelection?.profile
    ? selectedPreset
      ? `${artifactSelection.profile} — ${selectedPreset.name}`
      : artifactSelection.profile
    : selectedPreset?.name ?? "Ручная конфигурация (не именованный preset)";

  return (
    <main className="stepper-shell">
      <div className="stepper-topbar">
        <div>
          <p className="stepper-eyebrow">CACHESCOPE / INTERACTIVE</p>
          <h1>Пошаговая симуляция</h1>
          <p className="stepper-subtitle">
            Выполнение trace по одному событию с просмотром физического состояния
            кэша.
          </p>
        </div>
        <ProductNav current="stepper" />
      </div>

      {(artifactSelection || uploadedTracePath || runner.session) && (
        <section className="stepper-context" aria-label="Анализируемый schedule и cache profile">
          <div>
            <span>Источник</span>
            <strong>{artifactSelection ? `Эксперимент ${artifactSelection.experimentId}` : "Ручной artifact"}</strong>
          </div>
          <div>
            <span>Schedule / trace variant</span>
            <strong>{artifactSelection?.variant ?? runner.metadata?.source.variant ?? uploadedFileName ?? "загружается"}</strong>
          </div>
          <div>
            <span>Cache profile</span>
            <strong>{profileLabel}</strong>
          </div>
          <div className="stepper-context-levels">
            <span>Фактическая конфигурация сессии</span>
            <strong>{effectiveLevels.map((level) => `${level.name}: ${level.size_bytes} B, line ${level.line_size_bytes} B, ${level.associativity}-way, ${level.replacement_policy}, ${level.write_policy ?? "write_back"}/${level.allocation_policy ?? "write_allocate"}`).join(" · ")}</strong>
            {(runner.session?.hierarchy.inclusion_policies ?? hierarchy?.inclusion_policies)?.length ? <small>
              Hierarchy: {(runner.session?.hierarchy.inclusion_policies ?? hierarchy?.inclusion_policies)?.join(" → ")}; writeback clean: {String(runner.session?.hierarchy.writeback_clean ?? hierarchy?.writeback_clean ?? false)}; NINE fill: {runner.session?.hierarchy.non_inclusive_fill ?? hierarchy?.non_inclusive_fill ?? "all_levels"}
            </small> : null}
          </div>
        </section>
      )}

      {!runner.session && (
        <section className="tool-intro stepper-input-intro">
          <p className="product-eyebrow">MANUAL ARTIFACT TOOL</p>
          <h2>Что требуется Stepper</h2>
          <p>
            В основном сценарии открывайте Stepper кнопкой завершённого проекта —
            trace, MLIR stages и cache profile подставятся автоматически. Этот
            экран нужен для ручного исследования уже существующей трассы.
          </p>
          <div className="input-contract">
            <strong>Ручной вход</strong>
            <span><code>trace.json</code> — обязательная полная semantic trace</span>
            <span><code>input.mlir</code>, <code>01-transformed.mlir</code> и другие stages — необязательно, для source mapping</span>
            <span>Конфигурация кэша задаётся ниже перед созданием session</span>
          </div>
        </section>
      )}

      {!runner.session && (
        <section className="stepper-panel">
          <h2>Новая сессия</h2>

          <details className="stepper-advanced">
            <summary>Подключение к backend</summary>
            <label className="stepper-field">
              <span>Адрес backend API</span>
              <input
                value={baseUrl}
                onChange={(event) => setBaseUrl(event.target.value)}
                spellCheck={false}
              />
            </label>
          </details>

          <label className="stepper-field">
            <span>Файлы трассировки (trace.json + MLIR, опционально)</span>
            <input
              type="file"
              multiple
              accept=".json,.mlir,.txt"
              onChange={(event) => {
                const files = Array.from(event.target.files ?? []);
                setArtifactSelection(null);
                setHierarchy(undefined);
                void onFilesPicked(files);
              }}
              disabled={uploadBusy}
            />
            <small>
              Файлы передаются на runner по HTTP, поэтому работает и в Docker.
              Trace должен называться <code>trace.json</code>; остальные файлы
              сохраняются как MLIR-stages.
            </small>
          </label>

          {uploadBusy && (
            <p className="stepper-muted">Загрузка файлов на runner…</p>
          )}
          {uploadError && <p className="stepper-error">{uploadError}</p>}
          {uploadedTracePath && (
            <p className="stepper-muted">
              Загружено: <code>{uploadedFileName ?? "trace.json"}</code> +{" "}
              {uploadedStageCount}{" "}
              {plural(uploadedStageCount, "файл", "файла", "файлов")}.
              <br />
              trace_path: <code>{uploadedTracePath}</code>
            </p>
          )}

          <h3>Конфигурация кэша</h3>
          {levels.map((level, index) => {
            const currentPresetId = findMatchingPresetId(level);
            return (
              <div className="stepper-level-config" key={index}>
                <label>
                  <span>Имя</span>
                  <input
                    value={level.name}
                    onChange={(e) =>
                      updateLevel(index, { name: e.target.value })
                    }
                  />
                </label>
                <label>
                  <span>Пресет</span>
                  <select
                    value={currentPresetId ?? ""}
                    onChange={(e) => applyPreset(index, e.target.value)}
                  >
                    <option value="">Пользовательский</option>
                    {CACHE_PRESETS.map((preset) => (
                      <option key={preset.id} value={preset.id}>
                        {preset.name}
                      </option>
                    ))}
                  </select>
                </label>
                <label>
                  <span>Размер, байт</span>
                  <input
                    type="number"
                    min="1"
                    value={level.size_bytes}
                    onChange={(e) =>
                      updateLevel(index, {
                        size_bytes: numberValue(
                          e.target.value,
                          level.size_bytes,
                        ),
                      })
                    }
                  />
                </label>
                <label>
                  <span>Размер линии, байт</span>
                  <input
                    type="number"
                    min="1"
                    value={level.line_size_bytes}
                    onChange={(e) =>
                      updateLevel(index, {
                        line_size_bytes: numberValue(
                          e.target.value,
                          level.line_size_bytes,
                        ),
                      })
                    }
                  />
                </label>
                <label>
                  <span>Associativity</span>
                  <input
                    type="number"
                    min="1"
                    value={level.associativity}
                    onChange={(e) =>
                      updateLevel(index, {
                        associativity: numberValue(
                          e.target.value,
                          level.associativity,
                        ),
                      })
                    }
                  />
                </label>
                <label>
                  <span>Policy</span>
                  <select
                    value={level.replacement_policy}
                    onChange={(e) =>
                      updateLevel(index, {
                        replacement_policy: e.target
                          .value as LevelConfig["replacement_policy"],
                      })
                    }
                  >
                    <option>LRU</option>
                    <option>FIFO</option>
                    <option>Random</option>
                    <option>PLRU4</option>
                  </select>
                </label>
                {level.replacement_policy === "Random" && (
                  <label>
                    <span>Seed</span>
                    <input
                      value={level.random_seed ?? "0"}
                      onChange={(e) =>
                        updateLevel(index, { random_seed: e.target.value })
                      }
                    />
                  </label>
                )}
                {levels.length > 1 && (
                  <button
                    className="stepper-remove"
                    onClick={() => removeLevel(index)}
                  >
                    Удалить
                  </button>
                )}
              </div>
            );
          })}
          <div className="stepper-actions">
            <button
              className="stepper-button secondary"
              onClick={addLevel}
            >
              Добавить уровень
            </button>
            <button
              className="stepper-button primary"
              disabled={!canCreate || runner.busy}
              onClick={() =>
                uploadedTracePath &&
                void runner.create(uploadedTracePath, levels, hierarchy)
              }
            >
              Создать сессию
            </button>
          </div>
          <p className="stepper-note">
            Все уровни должны иметь одинаковый размер cache line. Число sets
            вычисляется backend. «Добавить уровень» создаёт следующий уровень
            вчетверо больше предыдущего — как в форме эксперимента.
          </p>
        </section>
      )}

      {runner.error && (
        <div className="stepper-error" role="alert">
          {runner.error}
        </div>
      )}
      {runner.busy && (
        <div className="stepper-loading">Выполняется запрос к runner…</div>
      )}

      {runner.session && (
        <>
          <section className="stepper-sessionbar">
            <div>
              <span className="stepper-muted">Сессия</span>
              <strong>{runner.session.session_id}</strong>
            </div>
            <div>
              <span className="stepper-muted">Статус</span>
              <strong>{runner.session.status}</strong>
            </div>
            <div>
              <span className="stepper-muted">Событие</span>
              <strong>
                {runner.progress?.cursor ?? runner.session.cursor} /{" "}
                {runner.session.total_events ?? "…"}
              </strong>
            </div>
            <div>
              <span className="stepper-muted">Ревизия</span>
              <strong>
                {runner.progress?.revision ?? runner.session.revision}
              </strong>
            </div>
            <button
              className="stepper-button secondary"
              disabled={runner.busy}
              onClick={() => void runner.clear()}
            >
              Закрыть сессию
            </button>
          </section>

          {runner.session.status === "failed" && (
            <div className="stepper-error">
              {runner.session.error?.message ?? "Сессия завершилась ошибкой"}
            </div>
          )}

          <section className="stepper-controls">
            <button
              className="stepper-button primary stepper-next"
              disabled={
                runner.busy ||
                !runner.progress ||
                runner.progress.status === "finished"
              }
              onClick={() => void runner.step()}
            >
              Следующий шаг →
            </button>

            <label
              className="stepper-field"
              style={{ display: "inline-grid", gap: 4, margin: 0 }}
            >
              <span>Пропустить шагов</span>
              <input
                type="number"
                min={1}
                max={runner.session.limits.max_step_count}
                value={skipCount}
                onChange={(e) => setSkipCount(Number(e.target.value))}
                disabled={
                  runner.busy ||
                  !runner.progress ||
                  runner.progress.status === "finished"
                }
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

            <button
              className="stepper-button secondary"
              disabled={runner.busy || !runner.progress}
              onClick={() => void runner.reset()}
            >
              Сбросить
            </button>

            <span className="stepper-note">
              «Следующий шаг» выполняет 1 событие. «Пропустить» выполняет
              введённое число событий одним batch-запросом.
            </span>
          </section>

          <div className="stepper-grid">
            <section className="stepper-panel">
              <div className="stepper-panel-heading">
                <h2>Текущее событие</h2>
                <span className="stepper-tag">
                  {currentEvent
                    ? `#${currentEvent.trace_event.sequence}`
                    : "нет события"}
                </span>
              </div>
              {currentEvent ? (
                <>
                  <dl className="stepper-kv">
                    <dt>Тип</dt>
                    <dd>{currentEvent.trace_event.kind}</dd>
                    <dt>Access ID</dt>
                    <dd>{currentEvent.trace_event.access_id}</dd>
                    <dt>Memref ID</dt>
                    <dd>{currentEvent.trace_event.memref_id}</dd>
                    <dt>Byte offset</dt>
                    <dd>{currentEvent.trace_event.byte_offset}</dd>
                    <dt>Размер</dt>
                    <dd>{currentEvent.trace_event.size_bytes} B</dd>
                    <dt>Адрес</dt>
                    <dd>{currentEvent.address}</dd>
                  </dl>
                  <h3>Результаты по уровням</h3>
                  {currentEvent.level_results.map((result) => (
                    <div className="stepper-event-level" key={result.level}>
                      <strong>L{result.level}</strong>
                      {result.lines.map((line, index) => (
                        <span key={index} className={line.hit ? "hit" : "miss"}>
                          {line.hit ? "HIT" : "MISS"} · set {line.set} · way{" "}
                          {line.way ?? "—"}
                        </span>
                      ))}
                    </div>
                  ))}
                  {currentEvent.evictions.length > 0 && (
                    <>
                      <h3>Вытеснения</h3>
                      {currentEvent.evictions.map((item, index) => (
                        <p className="stepper-small" key={index}>
                          L{item.level} · {item.line_address} ·{" "}
                          {item.dirty ? "dirty" : "clean"}
                        </p>
                      ))}
                    </>
                  )}
                </>
              ) : (
                <p className="stepper-muted">
                  Выполните первый шаг, чтобы увидеть событие.
                </p>
              )}
            </section>

            <section className="stepper-panel">
              <div className="stepper-panel-heading">
                <h2>MLIR source</h2>
                <span className="stepper-tag">
                  {currentAccess
                    ? `access ${currentAccess.access_id}`
                    : "source map"}
                </span>
              </div>
              {stages.length > 0 ? (
                <>
                  <label className="stepper-field">
                    <span>Stage</span>
                    <select
                      value={activeStage?.stage_id ?? selectedStage}
                      onChange={(e) => {
                        const stage = stages.find(
                          (item) => item.stage_id === e.target.value,
                        );
                        if (stage) void loadStage(stage);
                      }}
                    >
                      <option value="">Выберите stage</option>
                      {stages.map((stage) => (
                        <option key={stage.stage_id} value={stage.stage_id}>
                          {stage.stage_id} — {stage.path}
                        </option>
                      ))}
                    </select>
                  </label>
                  {activeStage && !stageText && !stageBusy && (
                    <button
                      className="stepper-button secondary"
                      onClick={() => void loadStage(activeStage)}
                    >
                      Загрузить MLIR
                    </button>
                  )}
                  {stageBusy && <p className="stepper-muted">Чтение файла…</p>}
                  {stageError && <p className="stepper-error">{stageError}</p>}
                  {currentStageLocation && (
                    <p className="stepper-note">
                      Позиция: строка {currentStageLocation.line}, колонка{" "}
                      {currentStageLocation.column};{" "}
                      {currentStageLocation.operation}
                    </p>
                  )}
                  {stageText && (
                    <pre className="stepper-source">
                      {stageText.split("\n").map((line, index) => (
                        <div
                          className={
                            currentStageLocation?.line === index + 1
                              ? "source-line active"
                              : "source-line"
                          }
                          key={index}
                        >
                          <span>{index + 1}</span>
                          <code>{line || " "}</code>
                        </div>
                      ))}
                    </pre>
                  )}
                </>
              ) : (
                <p className="stepper-muted">
                  В metadata нет source_map или сохранённых stage-файлов.
                </p>
              )}
              {currentAccess && (
                <p className="stepper-note">{currentAccess.source_location}</p>
              )}
            </section>
          </div>

          <section className="stepper-panel">
            <div className="stepper-panel-heading">
              <h2>Физическое состояние кэша</h2>
              <div className="stepper-tabs">
                {runner.session.levels.map((level) => (
                  <button
                    key={level.level}
                    className={selectedLevel === level.level ? "active" : ""}
                    onClick={() => setSelectedLevel(level.level)}
                  >
                    {level.name}
                  </button>
                ))}
              </div>
            </div>
            <div className="stepper-cache-meta">
              {runner.session.levels.find((level) => level.level === selectedLevel)
                ?.set_count ?? 0}{" "}
              sets ·{" "}
              {runner.session.levels.find((level) => level.level === selectedLevel)
                ?.associativity ?? 0}{" "}
              ways
            </div>
            <div className="stepper-cache-grid">
              {allSets.map((set) => (
                <div className="stepper-set" key={set.set}>
                  <strong>Set {set.set}</strong>
                  <div className="stepper-ways">
                    {set.ways.map((way) => {
                      const kind = highlights
                        .get(selectedLevel)
                        ?.get(`${set.set}-${way.way}`);
                      const cls =
                        "stepper-way" +
                        (way.valid ? " valid" : "") +
                        (kind === "hit" ? " way-hit" : "") +
                        (kind === "miss" ? " way-miss" : "");
                      return (
                        <div className={cls} key={way.way}>
                          <b>Way {way.way}</b>
                          <span>
                            {way.valid ? way.line_address : "INVALID"}
                          </span>
                          {way.valid && (
                            <small>{way.dirty ? "DIRTY" : "CLEAN"}</small>
                          )}
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
              <span className="stepper-tag">
                {runner.session.levels.length}{" "}
                {plural(
                  runner.session.levels.length,
                  "уровень",
                  "уровня",
                  "уровней",
                )}
              </span>
            </div>
            <p className="stepper-note">
              Hit rate по каждому сету для всех уровней кэша, накопленный по всем
              выполненным событиям. Наведите на ячейку, чтобы увидеть точные
              значения.
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
                Одна ячейка — один шаг. Зелёный — попадания, красный — промахи;
                если событие затронуло несколько cache line, ячейка делится по
                доле попаданий и промахов. Серые ячейки — шаги без обращений к
                этому уровню.
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

          {runner.progress && (
            <section className="stepper-panel">
              <h2>Накопительная статистика</h2>
              <div className="stepper-stats">
                <div>
                  <span>Hits</span>
                  <strong>{runner.progress.summary.totals.hits}</strong>
                </div>
                <div>
                  <span>Misses</span>
                  <strong>{runner.progress.summary.totals.misses}</strong>
                </div>
                <div>
                  <span>Evictions</span>
                  <strong>{runner.progress.summary.totals.evictions}</strong>
                </div>
                <div>
                  <span>Writebacks</span>
                  <strong>{runner.progress.summary.totals.writebacks}</strong>
                </div>
                <div>
                  <span>Hit rate</span>
                  <strong>
                    {runner.progress.summary.totals.hit_rate_percent == null
                      ? "—"
                      : `${runner.progress.summary.totals.hit_rate_percent.toFixed(2)}%`}
                  </strong>
                </div>
              </div>
            </section>
          )}
        </>
      )}

      <section className="stepper-panel stepper-logs">
        <div className="stepper-panel-heading">
          <h2>Журнал запросов к backend</h2>
          <div className="stepper-actions">
            <span className="stepper-tag">
              {requestLogs.length}{" "}
              {plural(requestLogs.length, "запись", "записи", "записей")}
            </span>
            <button
              className="stepper-button secondary"
              onClick={() => {
                const text = requestLogs
                  .map((log) =>
                    [
                      `[${log.time}]`,
                      `${log.method} ${log.url}`,
                      `Status: ${log.status ?? "NETWORK ERROR"}`,
                      `Duration: ${log.duration} ms`,
                      `Success: ${log.ok}`,
                      log.response,
                    ].join("\n"),
                  )
                  .join("\n\n-------------------------\n\n");
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
                  <span className="stepper-log-url">{log.url}</span>
                  <span
                    className={`stepper-log-status ${log.ok ? "ok" : "bad"}`}
                  >
                    {log.status ?? "NETWORK ERROR"}
                  </span>
                  <span className="stepper-log-duration">
                    {log.duration} ms
                  </span>
                </summary>
                <div className="stepper-log-details">
                  <p>
                    <strong>Результат:</strong>{" "}
                    {log.ok ? "Успешно" : "Ошибка"}
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
    </main>
  );
}
