import { useState } from "react";
import { FolderPicker } from "./components/FolderPicker";
import { RunCard } from "./components/RunCard";
import { ComparisonTable } from "./components/ComparisonTable";
import { loadEmbedded, loadExperiment } from "./loadExperiment";
import { describeConfig, groupByConfig } from "./metrics";
import type { LoadResult } from "./types";
import { StepperWorkspace } from "./features/stepper";
import { ExperimentForm } from "./components/ExperimentForm";

function isEmbeddedReport(): boolean {
  const el = document.getElementById("cachescope-data");
  return !!el?.textContent && el.textContent.trim() !== "null";
}

export default function App() {
  if (
    !isEmbeddedReport() &&
    new URLSearchParams(window.location.search).get("view") === "stepper"
  ) {
    return <StepperWorkspace />;
  }
  return <StatsView />;
}

function StatsView() {
  const [embedded] = useState<LoadResult | null>(loadEmbedded);
  const [result, setResult] = useState<LoadResult | null>(embedded);
  const [busy, setBusy] = useState(false);

  async function onFiles(files: FileList) {
    setBusy(true);
    try {
      setResult(await loadExperiment(files));
    } finally {
      setBusy(false);
    }
  }

  const groups = result ? groupByConfig(result.runs) : [];

  return (
    <main>
      <div className="app-heading">
        <h1>CacheScope</h1>
        {!embedded && <a href="/?view=stepper">Пошаговая симуляция →</a>}
      </div>

      {!embedded && <ExperimentForm onResult={setResult} />}
      {!embedded && <FolderPicker onFiles={onFiles} busy={busy} />}

      {result && result.errors.length > 0 && (
        <section className="errors">
          <h2>Ошибки чтения</h2>
          <ul>
            {result.errors.map((e) => (
              <li key={e.path}>
                <code>{e.path}</code>: {e.message}
              </li>
            ))}
          </ul>
        </section>
      )}

      {groups.map((runs) => (
        <section key={runs[0].path} className="group">
          <h2>{describeConfig(runs[0].stats.cache_config)}</h2>
          {runs.length > 1 && <ComparisonTable runs={runs} />}
          <div className="cards">
            {runs.map((r) => (
              <RunCard key={r.path} run={r} />
            ))}
          </div>
        </section>
      ))}
    </main>
  );
}
