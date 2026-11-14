import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

// /ws and /api go to `wrangler dev` (port 8787) during local development.
export default defineConfig({
  plugins: [react()],
  build: {
    rollupOptions: { input: { main: "index.html", admin: "admin.html" } },
  },
  server: {
    proxy: {
      "/api": "http://127.0.0.1:8787",
      "/auth": "http://127.0.0.1:8787",
      "/ws": { target: "ws://127.0.0.1:8787", ws: true },
    },
  },
});
