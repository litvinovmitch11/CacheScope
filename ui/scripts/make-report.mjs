
import { open, readdir, readFile, mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

function parseArgs(args) {
  const options = {
    input: null,
    output: "report.html",
  };

  // Поддержка старого синтаксиса:
  // npm run report -- ../output/experiment report.html
  if (args.length > 0 && !args[0].startsWith("-")) {
    options.input = args[0];
    if (args[1]) options.output = args[1];
    return options;
  }

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];

    if (arg === "--input" || arg === "-i") {
      options.input = args[++i];
    } else if (arg.startsWith("--input=")) {
      options.input = arg.slice("--input=".length);
    } else if (arg === "--output" || arg === "-o") {
      options.output = args[++i];
    } else if (arg.startsWith("--output=")) {
      options.output = arg.slice("--output=".length);
    } else if (arg === "--help" || arg === "-h") {
      console.log(
        "Использование: npm run report -- --input <experiment-dir> --output <report.html>",
      );
      process.exit(0);
    } else {
      throw new Error(`Неизвестный аргумент: ${arg}`);
    }
  }

  if (!options.input) {
    throw new Error("Укажи директорию эксперимента через --input.");
  }

  if (!options.output) {
    throw new Error("Укажи путь к отчёту через --output.");
  }

  return options;
}

const options = parseArgs(process.argv.slice(2));

const expDir = path.resolve(options.input);
const outFile = path.resolve(options.output);

const here = path.dirname(fileURLToPath(import.meta.url));
const templatePath = path.join(here, "..", "dist", "index.html");

async function* walk(dir) {
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const file = path.join(dir, entry.name);

    if (entry.isDirectory()) {
      yield* walk(file);
    } else if (entry.isFile()) {
      yield file;
    }
  }
}

const PREFIX = 64 * 1024;

function pickHeader(text) {
  const str = (key) =>
    new RegExp(`"${key}"\\s*:\\s*"([^"]*)"`).exec(text)?.[1];

  const num = (key) => {
    const match = new RegExp(
      `"${key}"\\s*:\\s*(\\d+)\\s*[,}]`,
    ).exec(text);

    return match ? Number(match[1]) : undefined;
  };

  const bool = (key) => {
    const match = new RegExp(
      `"${key}"\\s*:\\s*(true|false)\\b`,
    ).exec(text);

    return match ? match[1] === "true" : undefined;
  };

  const header = {
    schema: str("schema"),
    variant: str("variant"),
    total_events: num("total_events"),
    recorded_events: num("recorded_events"),
    truncated: bool("truncated"),
    valid: bool("valid"),
    complete: bool("complete"),
  };

  const required = [
    "schema",
    "variant",
    "total_events",
    "recorded_events",
    "truncated",
    "valid",
  ];

  return required.every((key) => header[key] !== undefined)
    ? header
    : null;
}

async function readTraceHeader(file) {
  const handle = await open(file, "r");

  try {
    const buffer = Buffer.alloc(PREFIX);
    const { bytesRead } = await handle.read(
      buffer,
      0,
      PREFIX,
      0,
    );

    const header = pickHeader(
      buffer.toString("utf8", 0, bytesRead),
    );

    if (header) return header;
  } finally {
    await handle.close();
  }

  // Запасной вариант, если заголовок не удалось прочитать.
  return JSON.parse(await readFile(file, "utf8"));
}

async function main() {
  const entries = [];
  let statsCount = 0;

  const expName = path.basename(expDir);

  for await (const file of walk(expDir)) {
    const relInExp = path
      .relative(expDir, file)
      .split(path.sep)
      .join("/");

    const base = path.basename(file);
    const inSimulation = relInExp.startsWith("simulation/");

    const isStats =
      inSimulation && base === "statistics.json";

    const isTrace =
      !inSimulation && base === "trace.json";

    if (!isStats && !isTrace) continue;

    const rel = `${expName}/${relInExp}`;

    try {
      const data = isTrace
        ? await readTraceHeader(file)
        : JSON.parse(await readFile(file, "utf8"));

      entries.push({ path: rel, data });

      if (isStats) statsCount++;
    } catch (error) {
      entries.push({
        path: rel,
        error:
          error instanceof Error
            ? error.message
            : String(error),
      });
    }
  }

  if (statsCount === 0) {
    throw new Error(
      `В ${expDir}/simulation не найдено ни одного statistics.json`,
    );
  }

  let template;

  try {
    template = await readFile(templatePath, "utf8");
  } catch {
    throw new Error(
      "Нет dist/index.html. Сначала выполните npm run build.",
    );
  }

  const dataTag =
    /(<script id="cachescope-data" type="application\/json">)[\s\S]*?(<\/script>)/;

  if (!dataTag.test(template)) {
    throw new Error(
      'В шаблоне нет <script id="cachescope-data">. Проверьте ui/index.html.',
    );
  }

  // Экранируем "<", чтобы данные не могли закрыть тег script.
  const json = JSON.stringify(entries).replace(/</g, "\\u003c");

  const html = template.replace(
    dataTag,
    (_, openingTag, closingTag) =>
      openingTag + json + closingTag,
  );

  await mkdir(path.dirname(outFile), { recursive: true });
  await writeFile(outFile, html, "utf8");

  console.log(`Готово: ${outFile}`);
  console.log(`Запусков: ${statsCount}`);
  console.log(`Trace: ${entries.length - statsCount}`);
}

main().catch((error) => {
  console.error(`Ошибка: ${error.message}`);
  process.exit(1);
});
