import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";

export default defineConfig({
  plugins: [vue()],
  base: "./",
  build: { outDir: "dist", assetsDir: "assets" },
  server: { port: 5173, proxy: { "/api": "http://127.0.0.1:8130", "/metrics": "http://127.0.0.1:8130" } },
});
