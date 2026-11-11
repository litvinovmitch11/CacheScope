import { expect, test } from "@playwright/test";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import { resolve } from "node:path";

const base = process.env.CACHESCOPE_E2E_URL ?? "http://127.0.0.1:9055";
const example = (name: string) => fileURLToPath(new URL(`../../examples/ui-upload/${name}`, import.meta.url));

test("upload MLIR and tensors, run real compiler and show the report without navigation", async ({ page }) => {
  test.setTimeout(180_000);
  const errors: string[] = [];
  page.on("pageerror", error => errors.push(error.message));
  await page.goto(base);
  await page.getByLabel("MLIR-файл", { exact: true }).setInputFiles(example("copy.mlir"));
  await page.getByLabel("Входные данные (NPZ)", { exact: true }).setInputFiles(example("inputs.npz"));
  await page.getByRole("button", { name: "Проверить файлы", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("Готов к запуску", { timeout: 60_000 });
  await expect(page.getByLabel("Функция", { exact: true })).toHaveValue("copy_kernel");
  await page.getByLabel("Аргумент 0", { exact: true }).selectOption("npz:source");
  await page.getByLabel("Аргумент 1", { exact: true }).selectOption("zeros");
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
  await page.goto(base);
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
