import { useEffect, useMemo, useState } from "react";
import { experimentRequest } from "../experimentApi";
import type {
  Experiment,
  TemplateResults,
  TorchProjectDescriptorSpec,
} from "../experimentApi";
import { loadResultEntries } from "../loadExperiment";
import { describeConfig, describeHierarchy, describeLevels, getLevels, groupByConfig } from "../metrics";
import { ComparisonTable } from "./ComparisonTable";
import { RunCard } from "./RunCard";

function profileFromPath(path: string, candidates: string[]): string | null {
  return [...candidates]
    .sort((left, right) => right.length - left.length)
    .find((name) => path.startsWith(`simulation/${name}--`)) ?? null;
}

function profilesFromRuns(paths: string[], candidates: string[]): string[] {
  return [...new Set(paths.map((path) => profileFromPath(path, candidates)).filter((name): name is string => !!name))];
}

export function ExperimentReport({ experimentId }: { experimentId: string | null }) {
  const [job, setJob] = useState<Experiment | null>(null);
  const [results, setResults] = useState<TemplateResults | null>(null);
  const [selectedVariant, setSelectedVariant] = useState("");
  const [selectedProfile, setSelectedProfile] = useState("");
  const [error, setError] = useState("");

  useEffect(() => {
    if (!experimentId) {
      setError("Не указан experiment. Откройте отчёт из завершённого проекта.");
      return;
    }
    Promise.all([
      experimentRequest<Experiment>(`/${experimentId}`),
      experimentRequest<TemplateResults>(`/${experimentId}/results`),
    ]).then(([nextJob, nextResults]) => {
      setJob(nextJob);
      setResults(nextResults);
      const firstSuccessful = Object.entries(nextResults.variants ?? {})
        .find(([, value]) => value.status === "completed")?.[0] ?? "";
      setSelectedVariant(firstSuccessful);
      if (nextJob.spec && "schema" in nextJob.spec && nextJob.spec.schema === "cachescope.torch-project.v1") {
        setSelectedProfile(Object.keys(nextJob.spec.cache_profiles)[0] ?? "");
      }
    }).catch((cause) => setError(cause instanceof Error ? cause.message : String(cause)));
  }, [experimentId]);

  useEffect(() => {
    if (!job || !results || window.location.hash !== "#stepper") return;
    const frame = window.requestAnimationFrame(() => {
      document.getElementById("stepper")?.scrollIntoView({
        behavior: "smooth",
        block: "start",
      });
    });
    return () => window.cancelAnimationFrame(frame);
  }, [job, results]);

  const loaded = useMemo(
    () => results ? loadResultEntries(results.entries) : null,
    [results],
  );
  const groups = loaded ? groupByConfig(loaded.runs) : [];
  const project = job?.spec && "schema" in job.spec && job.spec.schema === "cachescope.torch-project.v1"
    ? job.spec as TorchProjectDescriptorSpec
    : null;
  const successful = Object.entries(results?.variants ?? {})
    .filter(([, value]) => value.status === "completed")
    .map(([name]) => name);
  const profiles = Object.keys(project?.cache_profiles ?? {});
  const stepperEnabled = !!project && project.settings.stepper_events > 0 && successful.length > 0 && !!selectedProfile;
  const stepperUrl = selectedVariant && experimentId
    ? `/?view=stepper&experiment=${encodeURIComponent(experimentId)}&variant=${encodeURIComponent(selectedVariant)}&profile=${encodeURIComponent(selectedProfile)}&autostart=1`
    : "";

  return (
    <section className="experiment-report">
      <div className="report-heading">
        <div>
          <p className="product-eyebrow">COMPARATIVE EXPERIMENT REPORT</p>
          <h2>{project?.name ?? "Результаты эксперимента"}</h2>
          <p>Каждый compiler schedule проверен относительно eager Torch, затем воспроизведён на одинаковых cache profiles.</p>
        </div>
        <div className="report-heading-actions">
          <a className="button-link" href="/?view=validation">Как проверена модель</a>
          <a className="button-link" href="/">← Вернуться в проект</a>
        </div>
      </div>

      {error && <div className="errors" role="alert"><p>{error}</p></div>}
      {!error && (!job || !results) && <p className="muted">Загружаю результаты…</p>}

      {project && results && (
        <section className="report-correctness">
          <div>
            <p className="product-eyebrow">NUMERICAL CORRECTNESS GATE</p>
            <h3>Что означает «schedule корректен»</h3>
            <p>
              CacheScope один раз выполняет <code>model(*inputs)</code> в eager
              Torch и использует этот Tensor как эталон. Вывод каждого
              скомпилированного schedule проверяется вызовом
              <code> torch.testing.assert_close(compiled, eager, rtol, atol)</code>.
              Не прошедший проверку вариант не попадает в cache comparison.
            </p>
          </div>
          <dl>
            <div><dt>Входы</dt><dd>{results.contract?.input_shapes.map((shape) => `[${shape.join(" × ")}]`).join(", ") || "CPU float32, static shapes"}</dd></div>
            <div><dt>Выход</dt><dd>{results.contract ? `[${results.contract.output_shape.join(" × ")}] float32` : "один static float32 Tensor"}</dd></div>
            <div><dt>rtol</dt><dd><code>{results.contract?.validation.rtol ?? project.settings.validation.rtol}</code></dd></div>
            <div><dt>atol</dt><dd><code>{results.contract?.validation.atol ?? project.settings.validation.atol}</code></dd></div>
          </dl>
        </section>
      )}

      {results?.variants && (
        <div className="report-schedules">
          <div>
            <strong>Результат schedules</strong>
            <span>{successful.length} успешно из {Object.keys(results.variants).length}</span>
          </div>
          {Object.entries(results.variants).map(([name, value]) => (
            <div key={name} className={value.status === "completed" ? "variant-ok" : "variant-failed"}>
              <strong>{name}</strong>
              <span>{value.status === "completed" ? "compiled output совпал с eager Torch" : value.error}</span>
            </div>
          ))}
        </div>
      )}

      <section id="stepper" className="report-stepper">
        <div>
          <p className="product-eyebrow">INTERACTIVE EXPLORATION</p>
          <h3>Открыть schedule в Stepper</h3>
          <p>Выберите конкретную трансформацию. Stepper откроет именно её trace и MLIR stages.</p>
        </div>
        <label>
          Успешный schedule
          <select
            value={selectedVariant}
            disabled={!stepperEnabled}
            onChange={(event) => setSelectedVariant(event.target.value)}
          >
            {successful.map((name) => <option key={name} value={name}>{name}</option>)}
          </select>
        </label>
        <label>
          Cache profile
          <select
            value={selectedProfile}
            disabled={!stepperEnabled}
            onChange={(event) => setSelectedProfile(event.target.value)}
          >
            {profiles.map((name) => <option key={name} value={name}>{name}</option>)}
          </select>
        </label>
        {stepperEnabled
          ? <a className="button-link primary-report-action" href={stepperUrl}>Открыть выбранный Stepper →</a>
          : <span className="muted">Stepper выключен для этого запуска.</span>}
      </section>

      {loaded?.errors.length ? (
        <section className="errors">
          <h3>Ошибки чтения artifacts</h3>
          <ul>{loaded.errors.map((item) => <li key={item.path}><code>{item.path}</code>: {item.message}</li>)}</ul>
        </section>
      ) : null}

      {groups.map((runs) => {
        const groupProfiles = profilesFromRuns(runs.map((run) => run.path), profiles);
        const configuration = describeLevels(getLevels(runs[0].stats));
        const hierarchy = describeHierarchy(runs[0].stats);
        return (
          <section key={runs[0].path} className="group report-group">
            <div className="report-profile-heading">
              <p className="product-eyebrow">CACHE PROFILE</p>
              <h2>{groupProfiles.length ? groupProfiles.join(" / ") : describeConfig(runs[0].stats.cache_config)}</h2>
              <p className="muted">{configuration}{hierarchy ? ` · ${hierarchy}` : ""}</p>
              <p className="muted">Все schedules ниже воспроизводятся на этой конфигурации. Δ hit rate и Δ misses считаются относительно строки <code>baseline</code>.</p>
            </div>
            {runs.length > 1 && <ComparisonTable runs={runs} />}
            <div className="cards">{runs.map((run) => <RunCard key={run.path} run={run} />)}</div>
          </section>
        );
      })}
    </section>
  );
}
