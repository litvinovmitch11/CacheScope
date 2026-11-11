import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { viteSingleFile } from "vite-plugin-singlefile";
import { localFilesPlugin } from "./src/features/stepper-dev/localFilesPlugin";

export default defineConfig({
  plugins: [react(), viteSingleFile(), localFilesPlugin()],
});
