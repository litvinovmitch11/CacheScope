import { defineConfig } from "vite";
import react from "@vitejs/plugin-react"; // или react-swc, как у вас было
import { viteSingleFile } from "vite-plugin-singlefile";

export default defineConfig({
  plugins: [react(), viteSingleFile()],
});
