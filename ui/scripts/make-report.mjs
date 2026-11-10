import { open, readdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const [, , expDirArg, outFile = "report.html"] = process.argv;
if (!expDirArg) {
  console.error("usage: node scripts/make-report.mjs <output/experiment> [report.html]");
  process.exit(1);
}

const expDir = path.resolve(expDirArg);
const here = path.dirname(fileURLToPath(import.meta.url));
const templatePath = path.join(here, "..", "dist", "index.html");

async function* walk(dir) {
  for (const e of await readdir(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) yield* walk(p);
    else yield p;
  }
}

const PREFIX = 64 * 1024;

function pickHeader(text) {
  const str = (k) => new RegExp(`"${k}"\\s*:\\s*"([^"]*)"`).exec(text)?.[1];
  const num = (k) => {
    const m = new RegExp(`"${k}"\\s*:\\s*(\\d+)\\s*[,}]`).exec(text);
    return m ? Number(m[1]) : undefined;
  };
  const bool = (k) => {
    const m = new RegExp(`"${k}"\\s*:\\s*(true|false)\\b`).exec(text);
    return m ? m[1] === "true" : undefined;
  };
  const h = {
    schema: str("schema"),
    variant: str("variant"),
    producer: str("producer"),
    total_events: num("total_events"),
    recorded_events: num("recorded_events"),
    truncated: bool("truncated"),
  };
  return Object.values(h).every((v) => v !== undefined) ? h : null;
}

async function readTraceHeader(file) {
  const fh = await open(file, "r");
  try {
    const buf = Buffer.alloc(PREFIX);
    const { bytesRead } = await fh.read(buf, 0, PREFIX, 0);
    const head = pickHeader(buf.toString("utf8", 0, bytesRead));
    if (head) return head;
  } finally {
    await fh.close();
  }
  return JSON.parse(await readFile(file, "utf8")); // запасной путь
}

const entries = [];
let statsCount = 0;
const expName = path.basename(expDir);
for await (const file of walk(expDir)) {
  const relInExp = path.relative(expDir, file).split(path.sep).join("/");
  const base = path.basename(file);
  const inSim = relInExp.startsWith("simulation/");
  const isStats = inSim && base === "statistics.json";
  const isTrace = !inSim && base === "trace.json";
  if (!isStats && !isTrace) continue;

  const rel = `${expName}/${relInExp}`;
  try {
    const data = isTrace
      ? await readTraceHeader(file)
      : JSON.parse(await readFile(file, "utf8"));
    entries.push({ path: rel, data });
    if (isStats) statsCount++;
  } catch (e) {
    entries.push({ path: rel, error: String(e.message ?? e) });
  }
}
if (statsCount === 0) {
  console.error(`В ${expDir}/simulation не найдено ни одного statistics.json`);
  process.exit(1);
}

let template;
try {
  template = await readFile(templatePath, "utf8");
} catch {
  console.error("Нет dist/index.html. Сначала выполните: npm run build");
  process.exit(1);
}
if (!template.includes('id="cachescope-data"')) {
  console.error('В шаблоне нет <script id="cachescope-data">. Проверьте ui/index.html.');
  process.exit(1);
}

// "<" экранируем, чтобы данные не могли закрыть тег <script>
const json = JSON.stringify(entries).replace(/</g, "\\u003c");
const html = template.replace(
  /(<script id="cachescope-data" type="application\/json">)[\s\S]*?(<\/script>)/,
  (_, open, close) => open + json + close,
);

await writeFile(outFile, html);
console.log(
  `Готово: ${outFile} (${statsCount} запусков, ${entries.length - statsCount} trace)`,
);
