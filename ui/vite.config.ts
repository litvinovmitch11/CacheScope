import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { viteSingleFile } from "vite-plugin-singlefile";
import { localFilesPlugin } from "./src/features/stepper-dev/localFilesPlugin.js";
export default defineConfig({
  plugins: [react(), viteSingleFile(), localFilesPlugin()],
  server: { proxy: { "/api": "http://127.0.0.1:9055" } },
});
