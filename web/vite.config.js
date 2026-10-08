import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";

// `npm run dev` proxies the API to a running player (PLAYER_API, default the local one).
const target = process.env.PLAYER_API || "http://127.0.0.1:8130";

export default defineConfig({
  plugins: [vue()],
  base: "./",
  build: { outDir: "dist", assetsDir: "assets" },
  server: { port: 5173, proxy: { "/api": { target, ws: true }, "/metrics": target, "/livez": target, "/readyz": target, "/statusz": target } },
});
