import { Statistics, type LoadError, type LoadResult, type Run } from "./types";
import { checkInvariants } from "./metrics";

// output/<experiment>/simulation/<что-угодно>/statistics.json
// Подпуть после simulation/ не разбираем жёстко: в спеке пока не зафиксировано,
// где лежит variant (см. вопрос к авторам симулятора).
const STATS_RE = /(?:^|\/)simulation\/.+\/statistics\.json$/;

export async function loadExperiment(files: FileList): Promise<LoadResult> {
  const runs: Run[] = [];
  const errors: LoadError[] = [];

  const candidates = Array.from(files).filter((f) =>
    STATS_RE.test(f.webkitRelativePath),
  );

  if (candidates.length === 0) {
    errors.push({
      path: "(папка)",
      message:
        "Не найдено ни одного simulation/*/statistics.json. Выберите папку output/<experiment>/.",
    });
  }

  for (const file of candidates) {
    const path = file.webkitRelativePath;
    try {
      const json: unknown = JSON.parse(await file.text());
      const parsed = Statistics.safeParse(json);
      if (!parsed.success) {
        const message = parsed.error.issues
          .map((i) => `${i.path.join(".") || "(корень)"}: ${i.message}`)
          .join("; ");
        errors.push({ path, message });
        continue;
      }
      runs.push({
        path,
        stats: parsed.data,
        warnings: checkInvariants(parsed.data),
      });
    } catch (e) {
      errors.push({ path, message: e instanceof Error ? e.message : String(e) });
    }
  }

  runs.sort((a, b) => a.path.localeCompare(b.path));
  return { runs, errors };
}
