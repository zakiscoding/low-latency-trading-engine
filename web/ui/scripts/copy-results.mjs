// Copies benchmark result JSON from ../../results into public/results and writes an
// index, so the site shows exactly the numbers committed in the repo.
import { cpSync, existsSync, mkdirSync, readdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const src = new URL("../../../results/", import.meta.url).pathname.replace(/^\/([A-Z]:)/, "$1");
const dst = new URL("../public/results/", import.meta.url).pathname.replace(/^\/([A-Z]:)/, "$1");
mkdirSync(dst, { recursive: true });
const index = [];
if (existsSync(src)) {
  for (const machine of readdirSync(src, { withFileTypes: true })) {
    if (!machine.isDirectory()) continue;
    for (const f of readdirSync(join(src, machine.name))) {
      if (!f.endsWith(".json")) continue;
      mkdirSync(join(dst, machine.name), { recursive: true });
      cpSync(join(src, machine.name, f), join(dst, machine.name, f));
      index.push(`${machine.name}/${f}`);
    }
  }
}
writeFileSync(join(dst, "index.json"), JSON.stringify(index.sort(), null, 1));
console.log(`results: ${index.length} files`);
