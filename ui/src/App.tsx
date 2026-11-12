import { useState } from "react";
import { FolderPicker } from "./components/FolderPicker";
import { RunCard } from "./components/RunCard";
import { ComparisonTable } from "./components/ComparisonTable";
import { loadEmbedded, loadExperiment } from "./loadExperiment";
import { describeConfig, groupByConfig } from "./metrics";
import type { LoadResult } from "./types";
import { StepperWorkspace } from "./features/stepper";
import { ExperimentForm } from "./components/ExperimentForm";
import { ProductNav } from "./components/ProductNav";
import type { ProductView } from "./components/ProductNav";
import { ProjectStudio } from "./components/ProjectStudio";
import { ExperimentReport } from "./components/ExperimentReport";
import { ValidationHub } from "./components/ValidationHub";

function isEmbeddedReport(): boolean {
  const el = document.getElementById("cachescope-data");
  return !!el?.textContent && el.textContent.trim() !== "null";
}

export default function App() {
  const embedded = isEmbeddedReport();
  const requested = new URLSearchParams(window.location.search).get("view");
  if (!embedded && requested === "stepper") {
    return <StepperWorkspace />;
  }
  const view: Exclude<ProductView, "stepper"> =
    requested === "mlir" ? "mlir" :
    requested === "validation" ? "validation" :
    requested === "import" || requested === "results" ? "import" : "studio";
  if (!embedded && requested === "report") {
    return <StatsView embeddedReport={false} view="report" />;
  }
  return <StatsView embeddedReport={embedded} view={view} />;
}

function StatsView({
  embeddedReport,
  view,
}: {
  embeddedReport: boolean;
  view: Exclude<ProductView, "stepper">;
}) {
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
      <header className="product-header">
        <div>
          <p className="product-eyebrow">LOW-LEVEL ML OPTIMIZATION</p>
          <h1>CacheScope</h1>
          {!embeddedReport && (
            <p>Эксперименты, сравнение cache locality и пошаговый разбор.</p>
          )}
        </div>
        {!embeddedReport && <ProductNav current={view} />}
      </header>

      {!embeddedReport && view === "studio" && (
        <ProjectStudio />
      )}
      {!embeddedReport && view === "report" && (
        <ExperimentReport experimentId={new URLSearchParams(window.location.search).get("experiment")} />
      )}
      {!embeddedReport && view === "validation" && <ValidationHub />}
      {!embeddedReport && view === "mlir" && (
        <section className="tool-page">
          <div className="tool-intro">
            <p className="product-eyebrow">ADVANCED / PRECOMPILED INPUT</p>
            <h2>Запуск готового MLIR</h2>
            <p>
              Отдельный низкоуровневый инструмент. Используйте его, когда Torch
              export и Transform schedules уже выполнены вне CacheScope.
            </p>
            <div className="input-contract">
              <strong>Требуется на вход</strong>
              <span><code>program.mlir</code> — функция, аргументы которой являются статическими <code>memref&lt;…xf32&gt;</code>; текущий low-level runner не принимает dynamic shapes и другие element types.</span>
              <span><code>inputs.npz</code> — обычный NumPy-архив с именованными массивами, созданный через <code>np.savez(...)</code>. Каждый массив должен иметь dtype <code>float32</code>.</span>
              <span>После проверки файлов UI покажет аргументы выбранной MLIR-функции. Для каждого аргумента нужно выбрать NPZ-массив точно такой же формы либо заполнение нулями.</span>
              <code>{`np.savez("inputs.npz", image=np.zeros((1, 3, 224, 224), dtype=np.float32))`}</code>
            </div>
          </div>
          <ExperimentForm onResult={setResult} />
        </section>
      )}
      {!embeddedReport && view === "import" && (
        <section className="workspace-section">
          <p className="product-eyebrow">READ-ONLY ARTIFACT TOOL</p>
          <h2>Импорт готового результата</h2>
          <p className="muted">
            Этот раздел ничего не компилирует. Он открывает ранее созданный
            experiment bundle и строит сравнение вариантов.
          </p>
          <div className="input-contract">
            <strong>Выберите папку, содержащую</strong>
            <span><code>&lt;variant&gt;/trace.json</code> — заголовок trace</span>
            <span><code>simulation/&lt;profile&gt;--&lt;variant&gt;/statistics.json</code> — обязательная статистика</span>
            <span><code>summary.json</code> и остальные artifacts можно оставить в папке</span>
          </div>
          <FolderPicker onFiles={onFiles} busy={busy} />
        </section>
      )}

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
