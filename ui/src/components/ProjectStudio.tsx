import { useCallback, useEffect, useMemo, useState } from "react";
import { active, experimentRequest } from "../experimentApi";
import type {
  Experiment,
  ProjectCacheProfile,
  ProjectVariant,
  TemplateResults,
  TorchProjectDescriptorSpec,
  TorchProjectSpec,
} from "../experimentApi";
import { CACHE_PRESETS } from "../cachePresets";

const MODEL = `import torch
import torch.nn.functional as F

class DepthwiseBlock(torch.nn.Module):
    def forward(self, x, weight1, bias1, weight2, bias2):
        hidden = F.conv2d(x, weight1, bias1, groups=2)
        return F.conv2d(torch.relu(hidden), weight2, bias2, groups=2)

def create_model():
    return DepthwiseBlock().eval()
`;

const INPUTS = `import torch

def create_inputs():
    generator = torch.Generator().manual_seed(19)
    x = torch.randn(1, 2, 64, 64, generator=generator, dtype=torch.float32)
    weight1 = torch.randn(2, 1, 3, 3, generator=generator, dtype=torch.float32)
    bias1 = torch.randn(2, generator=generator, dtype=torch.float32)
    weight2 = torch.randn(2, 1, 3, 3, generator=generator, dtype=torch.float32)
    bias2 = torch.randn(2, generator=generator, dtype=torch.float32)
    return x, weight1, bias1, weight2, bias2
`;

const BASELINE = `module attributes {transform.with_named_sequence} {
  transform.named_sequence @__transform_main(
      %root: !transform.any_op {transform.readonly}) {
    transform.yield
  }
}
`;

const tiledTransform = (tile: number) => `module attributes {transform.with_named_sequence} {
  transform.named_sequence @__transform_main(
      %root: !transform.any_op {transform.readonly}) {
    %convolutions = transform.structured.match
      ops{["linalg.depthwise_conv_2d_nchw_chw"]} in %root
      : (!transform.any_op) -> !transform.any_op
    %first, %second = transform.split_handle %convolutions
      : (!transform.any_op) -> (!transform.any_op, !transform.any_op)
    %tiled, %height, %width = transform.structured.fuse %second
      tile_sizes [0, ${tile}, ${tile}, 0, 0, 0] {apply_cleanup}
      : (!transform.any_op) -> (!transform.any_op, !transform.any_op,
                                !transform.any_op)
    transform.yield
  }
}
`;

const DEFAULT_PROFILES = JSON.stringify(
  {
    "desktop-l1": ["L1:16384:64:4:lru"],
    "reference-cpu": [
      "L1:32768:64:8:lru",
      "L2:262144:64:8:lru",
      "L3:2097152:64:16:lru",
    ],
  },
  null,
  2,
);

type Draft = {
  name: string;
  model: string;
  inputs: string;
  variants: ProjectVariant[];
  profiles: string;
  stepperEvents: number;
  traceLimit: number;
  rtol: number;
  atol: number;
  timeout: number;
};
type Selection = { kind: "model" | "inputs" | "profiles" } | { kind: "variant"; index: number };

const exampleDraft = (): Draft => ({
  name: "Depthwise temporal blocking",
  model: MODEL,
  inputs: INPUTS,
  variants: [
    { name: "baseline", transform: BASELINE },
    { name: "tile-6", transform: tiledTransform(6) },
    { name: "tile-10", transform: tiledTransform(10) },
    { name: "tile-14", transform: tiledTransform(14) },
  ],
  profiles: DEFAULT_PROFILES,
  stepperEvents: 2048,
  traceLimit: 2_000_000,
  rtol: 1e-5,
  atol: 1e-5,
  timeout: 1800,
});

function initialDraft(): Draft {
  try {
    const saved = localStorage.getItem("cachescope.project.v1");
    if (!saved) return exampleDraft();
    const parsed = JSON.parse(saved) as Draft;
    const variants = Array.isArray(parsed.variants) ? parsed.variants : [];
    const legacyExample = parsed.name === "Depthwise temporal blocking" &&
      variants.length === 2 && variants[0]?.name === "baseline" && variants[1]?.name === "tile-10";
    return legacyExample ? exampleDraft() : parsed;
  } catch {
    return exampleDraft();
  }
}

const statusLabels: Record<Experiment["status"], string> = {
  preparing: "Подготовка", ready: "Готов", running: "Выполняется",
  cancelling: "Отмена…", completed: "Готово", failed: "Ошибка", cancelled: "Отменён",
};

export function ProjectStudio() {
  const [draft, setDraft] = useState<Draft>(initialDraft);
  const [selected, setSelected] = useState<Selection>(() =>
    new URLSearchParams(window.location.search).get("open") === "hardware-presets"
      ? { kind: "profiles" }
      : { kind: "model" },
  );
  const [job, setJob] = useState<Experiment | null>(null);
  const [busy, setBusy] = useState(false);
  const [importBusy, setImportBusy] = useState(false);
  const [error, setError] = useState("");
  const [variantResults, setVariantResults] = useState<TemplateResults["variants"]>();

  useEffect(() => { localStorage.setItem("cachescope.project.v1", JSON.stringify(draft)); }, [draft]);
  const acceptJob = useCallback((value: Experiment | null) => {
    setJob(value?.template_id === "local-torch-project" ? value : null);
  }, []);
  useEffect(() => {
    experimentRequest<{ experiments: Experiment[] }>().then((value) => {
      acceptJob(value.experiments[0] ?? null);
    }).catch(() => undefined);
  }, [acceptJob]);
  useEffect(() => {
    if (!job || !active(job)) return;
    let stopped = false;
    const timer = window.setInterval(() => {
      experimentRequest<Experiment>(`/${job.experiment_id}`).then((value) => {
        if (!stopped) acceptJob(value);
      }).catch((cause) => { if (!stopped) setError(cause instanceof Error ? cause.message : String(cause)); });
    }, 750);
    return () => { stopped = true; window.clearInterval(timer); };
  }, [acceptJob, job]);
  useEffect(() => {
    if (!job || job.status !== "completed") return;
    experimentRequest<TemplateResults>(`/${job.experiment_id}/results`).then((value) => {
      setVariantResults(value.variants);
    }).catch((cause) => setError(cause instanceof Error ? cause.message : String(cause)));
  }, [job]);

  const profiles = useMemo(() => {
    try {
      const value = JSON.parse(draft.profiles) as unknown;
      if (!value || Array.isArray(value) || typeof value !== "object") return null;
      const validLevel = (level: unknown) => !!level && typeof level === "object" &&
        typeof (level as { name?: unknown }).name === "string" &&
        Number.isInteger((level as { size_bytes?: unknown }).size_bytes) &&
        Number.isInteger((level as { line_size_bytes?: unknown }).line_size_bytes) &&
        Number.isInteger((level as { associativity?: unknown }).associativity) &&
        typeof (level as { replacement_policy?: unknown }).replacement_policy === "string";
      const validProfile = (profile: unknown) =>
        (Array.isArray(profile) && profile.length > 0 && profile.every((item) => typeof item === "string")) ||
        (!!profile && typeof profile === "object" && Array.isArray((profile as { levels?: unknown }).levels) &&
          ((profile as { levels: unknown[] }).levels.length > 0) &&
          (profile as { levels: unknown[] }).levels.every(validLevel));
      if (!Object.values(value).every(validProfile)) return null;
      return value as Record<string, ProjectCacheProfile>;
    } catch { return null; }
  }, [draft.profiles]);
  const valid = !!draft.name.trim() && !!draft.model.trim() && !!draft.inputs.trim() &&
    draft.name.length <= 128 && draft.variants.length > 0 && draft.variants.length <= 12 &&
    new Set(draft.variants.map((item) => item.name)).size === draft.variants.length &&
    draft.variants.every((item) => /^[A-Za-z0-9_-]{1,64}$/.test(item.name) && item.transform.trim()) &&
    !!profiles && Object.keys(profiles).length > 0 && Object.keys(profiles).length <= 8 &&
    Object.keys(profiles).every((name) => /^[A-Za-z0-9_-]{1,64}$/.test(name)) &&
    Number.isInteger(draft.stepperEvents) && draft.stepperEvents >= 0 && draft.stepperEvents <= 10_000 &&
    Number.isInteger(draft.traceLimit) && draft.traceLimit >= 1_000 && draft.traceLimit <= 10_000_000 &&
    Number.isFinite(draft.rtol) && draft.rtol >= 0 && Number.isFinite(draft.atol) && draft.atol >= 0 &&
    Number.isFinite(draft.timeout) && (draft.timeout === 0 || (draft.timeout >= 1 && draft.timeout <= 7_200));

  function update(patch: Partial<Draft>) { setDraft((current) => ({ ...current, ...patch })); }
  function updateVariant(index: number, patch: Partial<ProjectVariant>) {
    update({ variants: draft.variants.map((item, position) => position === index ? { ...item, ...patch } : item) });
  }
  function addVariant() {
    const index = draft.variants.length + 1;
    update({ variants: [...draft.variants, { name: `schedule-${index}`, transform: BASELINE }] });
    setSelected({ kind: "variant", index: draft.variants.length });
  }
  function addHardwarePreset(id: string) {
    const preset = CACHE_PRESETS.find((item) => item.id === id);
    if (!preset || !profiles) return;
    update({
      profiles: JSON.stringify({
        ...profiles,
        [preset.id]: {
          levels: preset.levels.map((level) => ({ ...level })),
          hierarchy: { ...preset.hierarchy, inclusion_policies: [...preset.hierarchy.inclusion_policies] },
        },
      }, null, 2),
    });
  }
  function removeVariant(index: number) {
    if (draft.variants.length === 1) return;
    update({ variants: draft.variants.filter((_, position) => position !== index) });
    setSelected({ kind: "model" });
  }
  async function importFolder(selectedFiles: FileList) {
    setImportBusy(true);
    setError("");
    try {
      const files = Array.from(selectedFiles);
      const fullPaths = files.map((file) => file.webkitRelativePath || file.name);
      const root = fullPaths[0]?.split("/")[0] ?? "Imported project";
      const relative = (path: string) => path.startsWith(`${root}/`) ? path.slice(root.length + 1) : path;
      const byPath = new Map(files.map((file, index) => [relative(fullPaths[index]), file]));
      const model = byPath.get("model.py");
      const inputs = byPath.get("inputs.py");
      const transforms = [...byPath.entries()]
        .filter(([path]) => /^transforms\/[^/]+\.mlir$/i.test(path))
        .sort(([left], [right]) => {
          const rank = (path: string): [number, number, string] => {
            const name = path.split("/").pop()?.replace(/\.mlir$/i, "") ?? path;
            if (name === "baseline") return [0, 0, name];
            const tile = /^tile-([0-9]+)$/.exec(name);
            return tile ? [1, Number(tile[1]), name] : [2, 0, name];
          };
          const a = rank(left); const b = rank(right);
          return a[0] - b[0] || a[1] - b[1] || a[2].localeCompare(b[2]);
        });
      if (!model || !inputs || transforms.length === 0) {
        throw new Error("В корне нужны model.py, inputs.py и хотя бы один transforms/*.mlir");
      }
      const profileFile = byPath.get("cache-profiles.json");
      const importedProfiles = profileFile ? await profileFile.text() : draft.profiles;
      JSON.parse(importedProfiles);
      const importedVariants = await Promise.all(transforms.map(async ([path, file]) => ({
        name: path.split("/").pop()!.replace(/\.mlir$/i, ""),
        transform: await file.text(),
      })));
      const modelSource = await model.text();
      const inputsSource = await inputs.text();
      setDraft((current) => ({
        ...current,
        name: root,
        model: modelSource,
        inputs: inputsSource,
        variants: importedVariants,
        profiles: importedProfiles,
      }));
      setSelected({ kind: "model" });
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Не удалось открыть проект");
    } finally {
      setImportBusy(false);
    }
  }
  async function start() {
    if (!valid || !profiles) return;
    setBusy(true); setError(""); setVariantResults(undefined);
    const spec: TorchProjectSpec = {
      schema: "cachescope.torch-project.v1", name: draft.name,
      files: { "model.py": draft.model, "inputs.py": draft.inputs },
      variants: draft.variants, cache_profiles: profiles,
      settings: { stepper_events: draft.stepperEvents, trace_limit: draft.traceLimit, validation: { rtol: draft.rtol, atol: draft.atol } },
      timeout_seconds: draft.timeout || null,
    };
    try {
      acceptJob(await experimentRequest<Experiment>("", {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(spec),
      }));
    } catch (cause) { setError(cause instanceof Error ? cause.message : String(cause)); }
    finally { setBusy(false); }
  }
  async function cancel() {
    if (!job) return;
    setBusy(true);
    try { acceptJob(await experimentRequest<Experiment>(`/${job.experiment_id}/cancel`, { method: "POST" })); }
    catch (cause) { setError(cause instanceof Error ? cause.message : String(cause)); }
    finally { setBusy(false); }
  }

  const running = active(job);
  const completedProject = job?.spec && "schema" in job.spec
    ? job.spec as TorchProjectDescriptorSpec
    : null;
  const stepperAvailable = completedProject?.schema === "cachescope.torch-project.v1" &&
    completedProject.settings.stepper_events > 0;
  return (
    <section className="studio">
      <ol className="workflow-rail" aria-label="Production pipeline">
        <li className="active"><span>1</span><div><strong>Проект</strong><small>Torch, inputs, schedules</small></div></li>
        <li className={job ? "active" : ""}><span>2</span><div><strong>Проверка и запуск</strong><small>export, correctness, trace</small></div></li>
        <li className={job?.status === "completed" ? "active" : ""}><span>3</span><div><strong>Сравнение</strong><small>cache profiles и дельты</small></div></li>
        <li className={job?.status === "completed" && stepperAvailable ? "active" : ""}><span>4</span><div><strong>Stepper</strong><small>event → cache → MLIR</small></div></li>
      </ol>
      <div className="studio-heading">
        <div><p className="product-eyebrow">LOCAL EXPERIMENT STUDIO</p><h2>Ваш Torch → MLIR эксперимент</h2>
          <p>Опишите Torch-блок и фиксированные inputs, добавьте schedules на Transform Dialect и сравните cache locality.</p></div>
        <div className="studio-heading-actions">
          <label className="project-folder-button">
            {importBusy ? "Открываю…" : "Открыть папку проекта"}
            <input
              type="file"
              multiple
              disabled={running || importBusy}
              ref={(element) => element?.setAttribute("webkitdirectory", "")}
              onChange={(event) => {
                if (event.target.files?.length) void importFolder(event.target.files);
                event.target.value = "";
              }}
            />
          </label>
          <button type="button" disabled={running} onClick={() => { setDraft(exampleDraft()); setSelected({ kind: "model" }); }}>Заполнить tile sweep</button>
        </div>
      </div>
      <div className="project-scope" aria-label="Контракт Torch-проекта и сравнения">
        <div>
          <strong>1. Что подаётся на вход</strong>
          <span><code>create_model()</code> возвращает <code>torch.nn.Module</code>, а <code>create_inputs()</code> — фиксированный набор CPU <code>float32</code> tensors для его <code>forward</code>.</span>
        </div>
        <div>
          <strong>2. Как проверяется результат</strong>
          <span>Сначала считается eager Torch output. Вывод каждого скомпилированного schedule сравнивается с ним через <code>torch.testing.assert_close(rtol, atol)</code>.</span>
        </div>
        <div>
          <strong>3. Что сравнивается дальше</strong>
          <span>Только прошедшие проверку schedules воспроизводятся на одинаковых cache profiles. Ошибочный schedule остаётся в отчёте с диагностикой.</span>
        </div>
        <div>
          <strong>Граница alpha</strong>
          <span>CPU, положительные static shapes, <code>float32</code>, один Tensor на выходе; поддерживаемые Torch operations ограничены экспортом torch-mlir и текущим MLIR pipeline.</span>
        </div>
      </div>
      <details className="project-contract">
        <summary>Как устроена папка проекта</summary>
        <div className="project-contract-grid">
          <pre>{`my-experiment/
  model.py
  inputs.py
  cache-profiles.json     # необязательно
  transforms/
    baseline.mlir
    tile-6.mlir
    tile-10.mlir
    tile-14.mlir`}</pre>
          <div>
            <p><code>model.py</code> экспортирует <code>create_model() -&gt; torch.nn.Module</code>.</p>
            <p><code>inputs.py</code> экспортирует <code>create_inputs() -&gt; tuple[Tensor, ...]</code>; порядок tensors совпадает с аргументами <code>forward</code>.</p>
            <p>Каждый MLIR-файл содержит <code>@__transform_main</code> и становится отдельным сравниваемым schedule.</p>
            <p>Без <code>cache-profiles.json</code> сохраняются профили из текущего draft.</p>
          </div>
        </div>
      </details>
      <div className="studio-project-name">
        <label>Название проекта<input value={draft.name} disabled={running} onChange={(event) => update({ name: event.target.value })} /></label>
      </div>
      <div className="studio-workspace">
        <aside className="studio-files" aria-label="Файлы проекта">
          <strong>PROJECT</strong>
          <button className={selected.kind === "model" ? "active" : ""} onClick={() => setSelected({ kind: "model" })}>model.py</button>
          <button className={selected.kind === "inputs" ? "active" : ""} onClick={() => setSelected({ kind: "inputs" })}>inputs.py</button>
          <div className="studio-folder"><span>transforms/</span><button title="Добавить schedule" disabled={running} onClick={addVariant}>＋</button></div>
          {draft.variants.map((variant, index) => (
            <button key={`${index}-${variant.name}`} className={selected.kind === "variant" && selected.index === index ? "active nested" : "nested"} onClick={() => setSelected({ kind: "variant", index })}>{variant.name}.mlir</button>
          ))}
          <button className={selected.kind === "profiles" ? "active" : ""} onClick={() => setSelected({ kind: "profiles" })}>cache-profiles.json</button>
        </aside>
        <div className="studio-editor">
          {selected.kind === "model" && <><div className="editor-bar"><strong>model.py</strong><span>create_model() → torch.nn.Module</span></div><textarea disabled={running} spellCheck={false} value={draft.model} onChange={(event) => update({ model: event.target.value })} /></>}
          {selected.kind === "inputs" && <><div className="editor-bar"><strong>inputs.py</strong><span>create_inputs() → tuple[Tensor, ...]</span></div><textarea disabled={running} spellCheck={false} value={draft.inputs} onChange={(event) => update({ inputs: event.target.value })} /></>}
          {selected.kind === "profiles" && <>
            <div className="editor-bar"><strong>cache-profiles.json</strong><span>NAME:SIZE:LINE:WAYS:POLICY</span></div>
            <textarea disabled={running} spellCheck={false} value={draft.profiles} onChange={(event) => update({ profiles: event.target.value })} />
            <div className="cache-profile-help">
              <span>Каждый ключ — отдельная cache model. Допустимы короткие строки или полная конфигурация <code>{`{ levels, hierarchy }`}</code>.</span>
              <button type="button" disabled={running} onClick={() => update({ profiles: DEFAULT_PROFILES })}>Вернуть рекомендуемые профили</button>
            </div>
            <div className="hardware-preset-picker" id="hardware-presets">
              <div><strong>Документированные hardware presets</strong><small>Геометрия и политики реального CPU в явно указанной области применимости.</small></div>
              {CACHE_PRESETS.map((preset) => (
                <button
                  type="button"
                  key={preset.id}
                  disabled={running || Object.hasOwn(profiles ?? {}, preset.id)}
                  title={preset.description}
                  onClick={() => addHardwarePreset(preset.id)}
                >
                  {Object.hasOwn(profiles ?? {}, preset.id) ? "Добавлен" : "Добавить"} {preset.name}
                </button>
              ))}
            </div>
            {!profiles && <p className="field-error">Нужен JSON-объект с непустыми массивами cache levels.</p>}
          </>}
          {selected.kind === "variant" && draft.variants[selected.index] && <>
            <div className="editor-bar schedule-bar"><label>Schedule <input disabled={running} value={draft.variants[selected.index].name} onChange={(event) => updateVariant(selected.index, { name: event.target.value })} /></label><button onClick={() => removeVariant(selected.index)} disabled={running || draft.variants.length === 1}>Удалить</button></div>
            <textarea disabled={running} spellCheck={false} value={draft.variants[selected.index].transform} onChange={(event) => updateVariant(selected.index, { transform: event.target.value })} />
          </>}
        </div>
      </div>
      <div className="studio-settings">
        <label>Stepper events<input type="number" min="0" max="10000" value={draft.stepperEvents} onChange={(event) => update({ stepperEvents: Number(event.target.value) })} /></label>
        <label>Trace limit<input type="number" min="1000" max="10000000" value={draft.traceLimit} onChange={(event) => update({ traceLimit: Number(event.target.value) })} /></label>
        <label title="Относительный допуск torch.testing.assert_close">rtol — относительный допуск<input type="number" step="any" min="0" value={draft.rtol} onChange={(event) => update({ rtol: Number(event.target.value) })} /></label>
        <label title="Абсолютный допуск torch.testing.assert_close">atol — абсолютный допуск<input type="number" step="any" min="0" value={draft.atol} onChange={(event) => update({ atol: Number(event.target.value) })} /></label>
        <label>Timeout, с<input type="number" min="1" max="7200" value={draft.timeout} onChange={(event) => update({ timeout: Number(event.target.value) })} /></label>
      </div>
      <div className="studio-gates">
        <strong>Автоматические проверки перед сравнением</strong>
        <span>Compiled output ≈ eager Torch output с указанными rtol/atol</span>
        <span>Полный semantic trace без process-address fallback</span>
        <span>Общий payload fingerprint и high-level provenance</span>
        <span>Одинаковые cache profiles для всех успешных schedules</span>
      </div>
      <div className="template-actions">
        <button className="primary-action" disabled={!valid || running || busy} onClick={() => void start()}>{job?.status === "completed" ? "Запустить снова" : "Запустить проект"}</button>
        {running && <button disabled={busy || job?.status === "cancelling"} onClick={() => void cancel()}>Отменить</button>}
        {job?.status === "completed" && job.artifacts && <>
          <a className="button-link" href={job.artifacts.report_url}>Открыть отчёт</a>
          {stepperAvailable && <a className="button-link" href={`${job.artifacts.report_url}#stepper`}>Настроить Stepper →</a>}
        </>}
      </div>
      {job && <div className="experiment-progress template-progress" role="status"><strong>{statusLabels[job.status]}</strong>{job.progress?.message && <span> — {job.progress.message}</span>}{job.progress?.current && job.progress.total && <progress value={job.progress.current} max={job.progress.total} />}{job.progress?.variant && <code>{job.progress.variant}</code>}</div>}
      {variantResults && <div className="variant-statuses">{Object.entries(variantResults).map(([name, value]) => <div key={name} className={value.status === "completed" ? "variant-ok" : "variant-failed"}><strong>{name}</strong><span>{value.status === "completed" ? "вывод совпал с eager Torch" : value.error}</span></div>)}</div>}
      {(error || job?.error) && <div className="errors" role="alert"><p>{error || job?.error?.message}</p>{job?.error?.diagnostic && <details><summary>Диагностика worker</summary><pre>{job.error.diagnostic}</pre></details>}</div>}
      {!valid && <p className="field-error">Проверьте имя проекта, уникальные имена schedules, исходники и cache profiles.</p>}
      <p className="muted">Код выполняется локально как доверенный пользовательский проект. Alpha: CPU, float32, static shapes, один Tensor на выходе.</p>
    </section>
  );
}
