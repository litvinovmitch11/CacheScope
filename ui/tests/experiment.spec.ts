import { expect, test } from "@playwright/test";
import type { Page } from "@playwright/test";
import { CACHE_PRESETS } from "../src/cachePresets";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";
import { mkdirSync, readFileSync } from "node:fs";
import { resolve } from "node:path";

const base = process.env.CACHESCOPE_E2E_URL ?? "http://127.0.0.1:9055";
const example = (name: string) => fileURLToPath(new URL(`../../examples/ui-upload/${name}`, import.meta.url));

async function prepareCopy(page: Page, inPlace = false) {
  await page.goto(base + "/?view=mlir");
  const program = inPlace ? { name: "inplace.mlir", mimeType: "text/plain",
    buffer: Buffer.from(readFileSync(example("copy.mlir"), "utf8").replace("memref.store %v, %dst", "%one = arith.constant 1.0 : f32\n        %next = arith.addf %v, %one : f32\n        memref.store %next, %src")) } : example("copy.mlir");
  await page.getByLabel("MLIR-файл", { exact: true }).setInputFiles(program);
  await page.getByLabel("Входные данные (NPZ)", { exact: true }).setInputFiles(example("inputs.npz"));
  await page.getByRole("button", { name: "Проверить файлы", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("Готов к запуску", { timeout: 60_000 });
  await expect(page.getByLabel("Функция", { exact: true })).toHaveValue("copy_kernel");
  await page.getByLabel("Аргумент 0", { exact: true }).selectOption("npz:source");
  await page.getByLabel("Аргумент 1", { exact: true }).selectOption("zeros");
  while (await page.locator(".cache-level").count() > 1)
    await page.getByRole("button", { name: "Удалить последний уровень", exact: true }).click();
  await page.getByLabel("L1 политика", { exact: true }).selectOption("LRU");
  await page.getByLabel("L1 размер", { exact: true }).fill("32768");
  await page.getByLabel("L1 строка", { exact: true }).fill("64");
  await page.getByLabel("L1 ways", { exact: true }).fill("8");
  await page.getByLabel("L1 запись", { exact: true }).selectOption("write_back");
  await page.getByLabel("L1 размещение записи", { exact: true }).selectOption("write_allocate");
}

test("one product navigation connects experiments, results, and stepper", async ({ page }) => {
  await page.goto(base);
  await expect(page.getByRole("heading", { name: "Ваш Torch → MLIR эксперимент", exact: true })).toBeVisible();
  await expect(page.getByRole("link", { name: "Проект", exact: true })).toHaveAttribute("aria-current", "page");
  for (const schedule of ["baseline.mlir", "tile-6.mlir", "tile-10.mlir", "tile-14.mlir"])
    await expect(page.getByRole("button", { name: schedule, exact: true })).toBeVisible();

  await page.getByRole("link", { name: "Валидация", exact: true }).click();
  await expect(page).toHaveURL(/\?view=validation$/);
  await expect(page.getByRole("heading", { name: "Почему результатам cache model можно доверять", exact: true })).toBeVisible();
  await expect(page.getByText("25 / 25 префиксов совпали", { exact: true })).toBeVisible();
  await expect(page.getByText("19 + 45 + 49 префиксов совпали", { exact: true })).toBeVisible();
  await expect(page.getByRole("link", { name: "Валидация", exact: true })).toHaveAttribute("aria-current", "page");

  await page.getByRole("link", { name: "Импорт результата", exact: true }).click();
  await expect(page).toHaveURL(/\?view=import$/);
  await expect(page.getByRole("heading", { name: "Импорт готового результата", exact: true })).toBeVisible();
  await expect(page.getByRole("link", { name: "Импорт результата", exact: true })).toHaveAttribute("aria-current", "page");

  await page.getByRole("link", { name: "Stepper", exact: true }).click();
  await expect(page).toHaveURL(/\?view=stepper$/);
  await expect(page.getByRole("heading", { name: "Пошаговая симуляция", exact: true })).toBeVisible();
  await expect(page.getByRole("link", { name: "Stepper", exact: true })).toHaveAttribute("aria-current", "page");

  await page.getByRole("link", { name: "MLIR / NPZ", exact: true }).click();
  await expect(page).toHaveURL(/\?view=mlir$/);
  await expect(page.getByRole("heading", { name: "Запустить MLIR", exact: true })).toBeVisible();

  await page.getByRole("link", { name: "Проект", exact: true }).click();
  await expect(page).toHaveURL(new RegExp("/$"));
  await expect(page.getByRole("heading", { name: "Ваш Torch → MLIR эксперимент", exact: true })).toBeVisible();
});

test("Project Studio adds a documented hardware preset as an executable profile", async ({ page }) => {
  await page.goto(base + "/?open=hardware-presets#hardware-presets");
  await expect(page.getByText("Документированные hardware presets", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Добавить IntelDX2 — встроенный L1, WT", exact: true }).click();
  const profiles = JSON.parse(await page.locator(".studio-editor textarea").inputValue());
  expect(profiles["intel-dx2-l1-wt"]).toEqual({
    levels: [{
      name: "L1", size_bytes: 8192, line_size_bytes: 16, associativity: 4,
      replacement_policy: "PLRU4", write_policy: "write_through",
      allocation_policy: "no_write_allocate",
    }],
    hierarchy: {
      inclusion_policies: [], writeback_clean: false,
      non_inclusive_fill: "all_levels",
    },
  });
});

test("upload MLIR and tensors, run real compiler and show the report without navigation", async ({ page }) => {
  test.setTimeout(180_000);
  const errors: string[] = [];
  page.on("pageerror", error => errors.push(error.message));
  await prepareCopy(page);
  await page.getByLabel(/ размер$/).first().fill("128");
  await page.getByLabel(/ ways$/).first().fill("2");
  await page.getByRole("button", { name: "Добавить уровень", exact: true }).click();
  await expect(page.getByLabel("Время, секунд", { exact: true })).toHaveValue("");
  await expect(page.getByLabel("Число событий", { exact: true })).toHaveValue("");
  await page.getByRole("button", { name: "Выполнить", exact: true }).click();
  await expect(page.getByRole("status")).toHaveText("Готово", { timeout: 120_000 });
  await expect(page.locator(".cards")).toContainText("93.75%");
  await page.getByText("Инструкции (2)", { exact: true }).click();
  await expect(page.locator(".instructions-table tbody tr")).toHaveCount(2);
  await page.locator(".card").getByRole("button", { name: "L2", exact: true }).click();
  await expect(page.locator(".instructions-table tbody tr")).toHaveCount(2);
  await expect(page).toHaveURL(new RegExp("/$"));
  await page.reload();
  await expect(page.getByRole("status")).toHaveText("Готово");
  await expect(page.locator(".cards")).toContainText("93.75%");
  await page.screenshot({ path: "test-results/experiment-report.png", fullPage: true });
  expect(errors).toEqual([]);
});

test("embedded HTML report still works without contacting the backend", async ({ page }) => {
  mkdirSync("test-results", { recursive: true });
  const output = resolve("test-results/offline-report.html");
  execFileSync(process.execPath, ["scripts/make-report.mjs", "--input", "fixtures/demo", "--output", output]);
  const apiRequests: string[] = [];
  page.on("request", request => { if (request.url().includes("/api/")) apiRequests.push(request.url()); });
  await page.goto("file://" + output);
  await expect(page.getByRole("heading", { name: "CacheScope", exact: true })).toBeVisible();
  await expect(page.locator(".card").first()).toBeVisible();
  await expect(page.getByRole("heading", { name: "Запустить MLIR", exact: true })).toHaveCount(0);
  expect(apiRequests).toEqual([]);
});

test("invalid MLIR shows an error on the same page and allows a new upload", async ({ page }) => {
  test.setTimeout(60_000);
  await page.goto(base + "/?view=mlir");
  await page.getByLabel("MLIR-файл", { exact: true }).setInputFiles({ name: "invalid.mlir", mimeType: "text/plain", buffer: Buffer.from("not MLIR") });
  await page.getByLabel("Входные данные (NPZ)", { exact: true }).setInputFiles(example("inputs.npz"));
  await page.getByRole("button", { name: "Проверить файлы", exact: true }).click();
  await expect(page.getByRole("status")).toHaveText(/Ошибка/);
  await expect(page.getByRole("alert")).toBeVisible();
  await expect(page.locator(".cards")).toHaveCount(0);
  await page.getByLabel("MLIR-файл", { exact: true }).setInputFiles(example("copy.mlir"));
  await page.getByRole("button", { name: "Проверить файлы", exact: true }).click();
  await expect(page.getByRole("status")).toHaveText("Готов к запуску", { timeout: 30_000 });
});


test("only documented CPU profiles are offered and manual settings remain editable", async ({ page }) => {
  test.setTimeout(60_000);
  await page.route("**/api/v1/experiments", route => route.request().method() === "GET"
    ? route.fulfill({ json: { experiments: [] } }) : route.continue());
  await prepareCopy(page);
  const picker = page.getByLabel("Профиль процессора", { exact: true });
  await expect(picker.locator("option")).toHaveText([
    "Ручная конфигурация", "IntelDX2 — встроенный L1, WT",
    "Write-Back Enhanced IntelDX4 — встроенный L1, WB",
  ]);
  await picker.selectOption("intel-dx2-l1-wt");
  await expect(page.getByLabel("L1 политика", { exact: true })).toHaveValue("PLRU4");
  await expect(page.getByLabel("L1 ways", { exact: true })).toHaveValue("4");
  await expect(page.getByLabel("L1 ways", { exact: true })).toHaveAttribute("max", "4");
  await expect(page.getByLabel("L1/L2 включение", { exact: true })).toHaveCount(0);
  await expect(page.getByRole("link", { name: "Запись / промах записи: Intel §7.1", exact: true })).toHaveAttribute("href", /#page=180$/);
  await picker.selectOption("intel-dx4-wb-l1");
  await expect(page.getByLabel("L1 размер", { exact: true })).toHaveValue("16384");
  await expect(page.getByLabel("L1 запись", { exact: true })).toHaveValue("write_back");
  await expect(page.getByLabel("L1 размещение записи", { exact: true })).toHaveValue("no_write_allocate");
  await picker.selectOption("");
  await page.getByLabel("L1 политика", { exact: true }).selectOption("LRU");
  await page.getByLabel("L1 строка", { exact: true }).fill("64");
  await expect(page.getByLabel("L1 размер", { exact: true })).toHaveValue("16384");
  await expect(page.getByRole("option", { name: /Phenom|Opteron|A8-3850|R10000|Pentium|Intel486/ })).toHaveCount(0);
  await page.getByLabel("L1 размер", { exact: true }).fill("65536");
  await expect(picker).toHaveValue("");
  await page.getByLabel("L1 запись", { exact: true }).selectOption("write_through");
  await page.getByLabel("L1 размещение записи", { exact: true }).selectOption("no_write_allocate");
  await page.getByRole("button", { name: "Добавить уровень", exact: true }).click();
  await expect(page.getByLabel("L1 размер", { exact: true })).toHaveValue("65536");
  await expect(page.getByLabel("L1 запись", { exact: true })).toHaveValue("write_through");
  await expect(page.getByLabel("L2 размер", { exact: true })).toHaveValue("262144");
  await page.getByLabel("L1/L2 включение", { exact: true }).selectOption("inclusive");
  await page.getByLabel("L1 политика", { exact: true }).selectOption("Random");
  await page.getByLabel("L1 seed", { exact: true }).fill("17");
  await page.getByLabel("L1 политика", { exact: true }).selectOption("LRU");
  await expect(page.getByLabel("L1 seed", { exact: true })).toHaveCount(0);
  await page.getByRole("button", { name: "Удалить последний уровень", exact: true }).click();
  await expect(page.getByLabel("L1/L2 включение", { exact: true })).toHaveCount(0);
  await expect(page.getByLabel("L1 размещение записи", { exact: true })).toHaveValue("no_write_allocate");
});

for (const preset of CACHE_PRESETS) {
  test(`real execution and reload preserve CPU profile: ${preset.name}`, async ({ page }) => {
    test.setTimeout(180_000);
    const errors: string[] = [];
    page.on("pageerror", error => errors.push(error.message));
    await prepareCopy(page, true);
    const picker = page.getByLabel("Профиль процессора", { exact: true });
    await picker.selectOption(preset.id);
    const reportResponse = page.waitForResponse(response => response.url().endsWith("/report") && response.status() === 200);
    await page.getByRole("button", { name: "Выполнить", exact: true }).click();
    await expect(picker).toBeDisabled();
    await expect(page.getByRole("status")).toHaveText("Готово", { timeout: 120_000 });
    const report = await (await reportResponse).json();
    expect(report.statistics.processed.trace_events).toBe(32);
    expect(report.statistics.totals.hits).toBe(28);
    expect(report.statistics.totals.misses).toBe(4);
    const through = preset.levels[0].write_policy === "write_through";
    expect(report.statistics.memory).toEqual({ reads: 4, writes: through ? 16 : 0,
      write_bytes: through ? 64 : 0, writebacks: 0 });
    await expect(page.locator(".cards")).toContainText("один уровень");
    await expect(page.locator(".cards")).toContainText("PLRU4");
    expect(report.statistics.hierarchy).toMatchObject(preset.hierarchy);
    expect(report.statistics.levels.map((level: { name: string; cache_config: object }) => ({ name: level.name, ...level.cache_config })))
      .toEqual(preset.levels.map(level => ({ ...level, set_count: level.size_bytes / level.line_size_bytes / level.associativity })));
    await expect(page.locator(".card .levels tbody tr")).toHaveCount(preset.levels.length);
    await page.screenshot({ path: `test-results/${preset.id}.png`, fullPage: true });
    await page.reload();
    await expect(page.getByRole("status")).toHaveText("Готово");
    await expect(picker).toHaveValue(preset.id);
    for (const level of preset.levels) {
      await expect(page.getByLabel(`${level.name} размер`, { exact: true })).toHaveValue(String(level.size_bytes));
    }
    expect(errors).toEqual([]);
  });
}

test("inclusive back invalidations are visible in a real UI report", async ({ page }) => {
  test.setTimeout(180_000);
  await prepareCopy(page);
  await page.getByRole("button", { name: "Добавить уровень", exact: true }).click();
  await page.getByLabel("L1 размер", { exact: true }).fill("128");
  await page.getByLabel("L1 ways", { exact: true }).fill("2");
  await page.getByLabel("L2 размер", { exact: true }).fill("64");
  await page.getByLabel("L2 ways", { exact: true }).fill("1");
  await page.getByLabel("L1/L2 включение", { exact: true }).selectOption("inclusive");
  const response = page.waitForResponse(r => r.url().endsWith("/report") && r.status() === 200);
  await page.getByRole("button", { name: "Выполнить", exact: true }).click();
  await expect(page.getByRole("status")).toHaveText("Готово", { timeout: 120_000 });
  const report = await (await response).json();
  expect(report.statistics.hierarchy.inclusion_policies).toEqual(["inclusive"]);
  expect(report.statistics.memory).toEqual({ reads: 32, writes: 15, writebacks: 15, write_bytes: 960 });
  expect(report.statistics.levels[0].totals.back_invalidations).toBe(31);
  await expect(page.locator(".cards")).toContainText("back invalidations");
  await expect(page.locator(".cards")).toContainText("inclusive");
  await page.screenshot({ path: "test-results/cache-policies-inclusive.png", fullPage: true });
});

test("mixed hierarchy policies survive execution and reload", async ({ page }) => {
  test.setTimeout(180_000);
  await prepareCopy(page);
  await page.getByRole("button", { name: "Добавить уровень", exact: true }).click();
  await page.getByLabel("L1 запись", { exact: true }).selectOption("write_through");
  await page.getByLabel("L1 размещение записи", { exact: true }).selectOption("no_write_allocate");
  await page.getByRole("button", { name: "Добавить уровень", exact: true }).click();
  await page.getByLabel("L1/L2 включение", { exact: true }).selectOption("non_inclusive");
  await page.getByLabel("L2/L3 включение", { exact: true }).selectOption("inclusive");
  await page.getByLabel("Передавать чистые вытеснения", { exact: true }).check();
  await page.getByLabel("Заполнение NINE", { exact: true }).selectOption("on_eviction");
  const response = page.waitForResponse(r => r.url().endsWith("/report") && r.status() === 200);
  await page.getByRole("button", { name: "Выполнить", exact: true }).click();
  await expect(page.getByRole("status")).toHaveText("Готово", { timeout: 120_000 });
  const report = await (await response).json();
  expect(report.statistics.hierarchy).toMatchObject({ inclusion_policy: "mixed", inclusion_policies: ["non_inclusive", "inclusive"], write_policy: "mixed", allocation_policy: "mixed", writeback_clean: true, non_inclusive_fill: "on_eviction" });
  expect(report.statistics.levels[0].cache_config).toMatchObject({ write_policy: "write_through", allocation_policy: "no_write_allocate" });
  await page.reload();
  await expect(page.getByRole("status")).toHaveText("Готово");
  await expect(page.getByLabel("Профиль процессора", { exact: true })).toHaveValue("");
  await expect(page.getByLabel("L1 запись", { exact: true })).toHaveValue("write_through");
  await expect(page.getByLabel("L1 размещение записи", { exact: true })).toHaveValue("no_write_allocate");
  await expect(page.getByLabel("L2/L3 включение", { exact: true })).toHaveValue("inclusive");
  await expect(page.getByLabel("Передавать чистые вытеснения", { exact: true })).toBeChecked();
  await expect(page.getByLabel("Заполнение NINE", { exact: true })).toHaveValue("on_eviction");
});

test("a saved removed CPU profile reloads as an editable manual configuration", async ({ page }) => {
  test.setTimeout(180_000);
  await prepareCopy(page);
  const list = await (await page.request.get(base + "/api/v1/experiments")).json();
  const id = list.experiments[0].experiment_id;
  const response = await page.request.post(base + `/api/v1/experiments/${id}/run`, {
    data: {
      function: "copy_kernel", bindings: [{ source: "npz", key: "source" }, { source: "zeros" }],
      levels: [{ name: "L1", size_bytes: 8192, line_size_bytes: 16, associativity: 4, replacement_policy: "LRU", write_policy: "write_through", allocation_policy: "no_write_allocate" }],
      hierarchy: { inclusion_policies: [], writeback_clean: false, non_inclusive_fill: "all_levels" },
      cache_profile: "intel486-dx", timeout_seconds: null, trace_limit: null,
    },
  });
  expect(response.status()).toBe(202);
  await page.reload();
  await expect(page.getByRole("status")).toHaveText("Готово", { timeout: 120_000 });
  await expect(page.getByLabel("Профиль процессора", { exact: true })).toHaveValue("");
  await expect(page.getByLabel("L1 размер", { exact: true })).toHaveValue("8192");
  await expect(page.getByLabel("L1 строка", { exact: true })).toHaveValue("16");
  await expect(page.getByLabel("L1 запись", { exact: true })).toHaveValue("write_through");
  await expect(page.getByLabel("L1 размещение записи", { exact: true })).toHaveValue("no_write_allocate");
  await expect(page.locator(".cards")).toContainText("write_through");
  await page.getByLabel("MLIR-файл", { exact: true }).setInputFiles(example("copy.mlir"));
  await page.getByLabel("Входные данные (NPZ)", { exact: true }).setInputFiles(example("inputs.npz"));
  await page.getByRole("button", { name: "Проверить файлы", exact: true }).click();
  await expect(page.getByRole("status")).toHaveText("Готов к запуску", { timeout: 60_000 });
  await page.getByLabel("L1 политика", { exact: true }).selectOption("FIFO");
  await expect(page.getByLabel("L1 размер", { exact: true })).toHaveValue("8192");
});
