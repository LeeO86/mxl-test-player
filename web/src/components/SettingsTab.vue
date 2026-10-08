<script setup>
// Settings (§9, §10): every setting with its value and origin (read-only: the player reads them at
// start), and the operator state as one JSON document (export, import).
import { computed, onMounted, ref } from "vue";
import { api, copyText, download } from "../api.js";
import { live, refreshLists } from "../store.js";

const settings = ref([]);
const exportDoc = ref("");
const exportMsg = ref("");
const importText = ref("");
const importMsg = ref({ kind: "", text: "" });

const GROUPS = [
  { title: "Player", test: /^(PLAYER_(FORMAT|OUTPUTS|AUDIO_CHANNELS|PREROLL_FRAMES|RAM_)|SHUTDOWN_)/ },
  { title: "Library and conversion", test: /^PLAYER_(LIBRARY_DIR|IMPORT_DIR|CONVERT|UPLOAD|SPRITE)/ },
  { title: "MXL", test: /^MXL_/ },
  { title: "NMOS", test: /^(NMOS_|HOST_ID)/ },
  { title: "Files, web and process", test: /./ },
];
// One line per key, from the README settings table.
const DESCRIPTIONS = {
  CONFIG_DIR: "Only directory the process writes for its own state.",
  PLAYER_CONFIG: "Configuration file (--config wins).",
  PLAYER_STATE: "State file: outputs, presets and playlists.",
  PLAYER_FORMAT: "Platform format of every output without its own.",
  PLAYER_OUTPUTS: "Number of outputs (1–16).",
  PLAYER_AUDIO_CHANNELS: "Default audio channels per output.",
  PLAYER_LIBRARY_DIR: "Media library (mezzanines, stills, sprites).",
  PLAYER_IMPORT_DIR: "Watched import directory (empty: off).",
  PLAYER_CONVERT_CONCURRENCY: "Conversions at once.",
  PLAYER_RAM_CLIP_MAX_S: "Longest clip kept in RAM.",
  PLAYER_RAM_BUDGET_MB: "RAM budget for clips.",
  PLAYER_PREROLL_FRAMES: "Frames prepared before play.",
  PLAYER_FONT_DIR: "Font directory (empty: bundled).",
  PLAYER_WEB_ROOT: "This web UI (empty: bundled).",
  PLAYER_UPLOAD_LIMIT_GB: "Largest upload.",
  PLAYER_SPRITE_MAX_PX: "Largest moving-box sprite.",
  MXL_DOMAIN_SCAN_PATH: "Parent of the domain directories.",
  MXL_OUTPUT_DOMAIN_DIR: "This player's domain (empty: <scan>/player-<seed>).",
  MXL_OUTPUT_DOMAIN_ID: "Domain id when the domain is created (empty: from the seed).",
  MXL_HISTORY_DURATION_NS: "Ring length, written when options.json is created.",
  MXL_CLEANUP_ON_EXIT: "Remove this domain on shutdown.",
  NMOS_REGISTRY_ADDRESS: "Registration API host (empty: no registration).",
  NMOS_REGISTRY_PORT: "Registration API port.",
  NMOS_QUERY_ADDRESS: "Query API host (/readyz).",
  NMOS_QUERY_PORT: "Query API port.",
  NMOS_DNS_SD: "Always false: this build has no DNS-SD.",
  NMOS_PORT: "IS-04 node API and IS-05 connection API.",
  NMOS_SEED: "Root of every NMOS id and the default domain id.",
  NMOS_LABEL: "Node and device label.",
  NMOS_TAGS: "Tags on the node and device.",
  NMOS_HOST_ADDRESS: "IPv4 address announced to NMOS.",
  HOST_ID: "Seed prefix when NMOS_SEED is empty.",
  SHUTDOWN_TIMEOUT_S: "Bound for the SIGTERM cleanup.",
  WEB_PORT: "This UI, the API, probes and metrics.",
};
const ORIGIN = {
  environment: { text: "ENV", cls: "env" },
  file: { text: "FILE", cls: "file" },
  argument: { text: "ARG", cls: "arg" },
  default: { text: "DEFAULT", cls: "default" },
};
const groups = computed(() => {
  const left = [...settings.value];
  return GROUPS.map((g) => {
    const items = left.filter((s) => g.test.test(s.key));
    items.forEach((s) => left.splice(left.indexOf(s), 1));
    return { title: g.title, items };
  }).filter((g) => g.items.length);
});

async function load() {
  try {
    settings.value = (await api.get("/api/v1/config")).settings || [];
    exportDoc.value = JSON.stringify(await api.get("/api/v1/config/export"), null, 2);
  } catch (e) {
    live.actionError = e.message;
  }
}
onMounted(load);

async function copy() {
  exportMsg.value = (await copyText(exportDoc.value)) ? "Copied." : "Copy failed; select the text and copy it by hand.";
}
function onFile(ev) {
  ev.target.files?.[0]?.text().then((t) => (importText.value = t));
}
async function doImport() {
  try {
    JSON.parse(importText.value);
  } catch (e) {
    importMsg.value = { kind: "err", text: `Not JSON: ${e.message}` };
    return;
  }
  if (!confirm("Import? The outputs take the document's sources, transport, burn-ins and sender enables; presets and playlists are replaced.")) return;
  try {
    await api.post("/api/v1/config/import", importText.value);
    importMsg.value = { kind: "ok", text: "Imported. Outputs, presets and playlists are restored; deployment settings are unchanged." };
    refreshLists();
    load();
  } catch (e) {
    importMsg.value = { kind: "err", text: e.message };
  }
}
const seed = computed(() => settings.value.find((s) => s.key === "NMOS_SEED")?.value || "player");
</script>

<template>
  <div class="panel">
    <h3>Configuration</h3>
    <p class="note" style="margin: 0">
      Precedence: environment (ENV), then the configuration file (FILE, or ARG for <code>--config</code>), then the default. The player reads them at
      start: change them in its deployment and restart it. Output labels, formats and key modes can also be set per output in the file
      (<code>outputs</code>).
    </p>
  </div>
  <div v-for="g in groups" :key="g.title" class="panel">
    <h3>{{ g.title }}</h3>
    <table>
      <thead>
        <tr><th style="width: 34%">Key</th><th>Value</th><th style="width: 6rem">Origin</th></tr>
      </thead>
      <tbody>
        <tr v-for="s in g.items" :key="s.key">
          <td>
            <code>{{ s.key }}</code>
            <div class="desc">{{ DESCRIPTIONS[s.key] || "" }}</div>
          </td>
          <td style="overflow-wrap: anywhere">{{ s.value === "" ? "–" : s.value }}</td>
          <td><span class="badge" :class="ORIGIN[s.source]?.cls">{{ ORIGIN[s.source]?.text || s.source }}</span></td>
        </tr>
      </tbody>
    </table>
  </div>

  <div class="grid two">
    <div class="panel">
      <h3>Export</h3>
      <p class="note">One JSON document: deployment fields (for information) and the operator state (outputs, presets, playlists). No secrets.</p>
      <div class="actions" style="margin-top: 0">
        <span class="msg ok" style="margin: 0">{{ exportMsg }}</span>
        <span class="spacer"></span>
        <button class="btn secondary" @click="load">Reload</button>
        <button class="btn secondary" :disabled="!exportDoc" @click="copy">Copy</button>
        <button class="btn" :disabled="!exportDoc" @click="download(`mxl-test-player-${seed}.json`, exportDoc + '\n')">Download</button>
      </div>
      <pre style="max-height: 320px">{{ exportDoc }}</pre>
    </div>
    <div class="panel">
      <h3>Import</h3>
      <p class="note">An exported document (or its <code>state</code>). Ports, registry and seed are not changed.</p>
      <input type="file" accept="application/json,.json" aria-label="Open an exported JSON file" @change="onFile" />
      <textarea v-model="importText" aria-label="Exported JSON" placeholder='{"version": 1, "state": {...}}' style="margin-top: 0.5rem"></textarea>
      <div class="actions">
        <span class="msg" :class="importMsg.kind" style="margin: 0">{{ importMsg.text }}</span>
        <span class="spacer"></span>
        <button class="btn" :disabled="!importText.trim()" @click="doImport">Import</button>
      </div>
    </div>
  </div>
</template>
