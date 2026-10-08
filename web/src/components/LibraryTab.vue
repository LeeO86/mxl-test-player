<script setup>
// Library (§6, §9): chunked upload with the conversion options, the conversion jobs, and the
// items with thumbnail, original, conversions, tags, play on an output, edit, re-convert, delete.
import { computed, ref, watch } from "vue";
import Pill from "./Pill.vue";
import Segmented from "./Segmented.vue";
import { ALPHA_MODES, api, fmtBytes, fmtSeconds, FITS, FPS_MODES } from "../api.js";
import { act, itemsById, live, outputLabel, playItem, refreshLists, selectedOutput, uploadFiles, uploads } from "../store.js";

// Search: GET /api/v1/library?q= (name or tag), type filter here.
const query = ref("");
const found = ref(null); // ids from the last search, null without one
let searchTimer = null;
watch(query, (q) => {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(async () => {
    if (!q.trim()) {
      found.value = null;
      return;
    }
    const list = await act(() => api.get(`/api/v1/library?q=${encodeURIComponent(q.trim())}`));
    if (Array.isArray(list)) found.value = new Set(list.map((i) => i.id));
  }, 250);
});
const type = ref("all");
const TYPES = [
  { value: "all", label: "All" },
  { value: "video", label: "Videos" },
  { value: "still", label: "Stills" },
  { value: "sprite", label: "Sprites" },
];
const rows = computed(() => live.library.filter((i) => (type.value === "all" || i.type === type.value) && (!found.value || found.value.has(i.id))));

const STATUS_KIND = { ready: "ok", queued: "neutral", running: "warn", failed: "bad" };
const usedBy = (item) => live.outputs.filter((o) => o.source?.item_id === item.id || (o.source?.type === "playlist" && live.playlists.find((p) => p.id === o.source.playlist_id)?.entries?.some((e) => e.item_id === item.id)));
function original(item) {
  const o = item.original || {};
  if (item.type !== "video") return `${o.width}×${o.height} ${o.codec || ""}`;
  const [n, d] = String(o.rate || "0/1").split("/").map(Number);
  const rate = d ? Math.round((n / d) * 100) / 100 : 0;
  return `${o.width}×${o.height} ${o.scan === "progressive" ? "p" : "i"}${rate} ${o.codec || ""} · ${fmtSeconds(o.duration)} · ${o.audio_channels || 0} ch ${o.audio_codec || ""}`;
}

// Upload
const dragging = ref(false);
const fileInput = ref(null);
function onFiles(list) {
  if (list?.length) uploadFiles([...list]);
  if (fileInput.value) fileInput.value.value = "";
}
const uploadState = (u) =>
  u.state === "sending" ? `sending ${Math.round((u.sent / Math.max(1, u.size)) * 100)} %` : u.state === "ingesting" ? "reading the file…" : u.state === "done" ? "uploaded, converting" : u.error;

// Jobs
const showDone = ref(false);
const jobs = computed(() => [...live.jobs].reverse().filter((j) => showDone.value || j.state !== "done"));

// Edit (name, tags) and re-convert dialogs
const dialog = ref(null);
const editing = ref(null); // { mode: "edit" | "reconvert", item, name, tags, options }
function openEdit(item, mode) {
  editing.value = {
    mode,
    item,
    name: item.name,
    tags: (item.tags || []).join(", "),
    options: { fit: item.fit, fps_mode: item.fps_mode, alpha_mode: item.alpha_mode, loudness: item.loudness, crossfade_ms: item.crossfade_ms, map_channels: item.map_channels },
  };
  dialog.value.showModal();
}
async function saveEdit() {
  const e = editing.value;
  const id = encodeURIComponent(e.item.id);
  let ok;
  if (e.mode === "edit") {
    const tags = e.tags.split(",").map((t) => t.trim()).filter(Boolean);
    ok = await act(() => api.patch(`/api/v1/library/${id}`, { name: e.name.trim(), tags }), `Saved ${e.name.trim()}.`);
  } else {
    const options = { ...e.options, crossfade_ms: Number(e.options.crossfade_ms) || 0, map_channels: Number(e.options.map_channels) || 0 };
    ok = await act(() => api.post(`/api/v1/library/${id}/reconvert`, options), `Re-converting ${e.item.name}.`);
  }
  if (ok) {
    dialog.value.close();
    refreshLists();
  }
}
async function remove(item) {
  const users = usedBy(item).map((o) => o.label);
  const warn = users.length ? `\n${users.join(", ")} use${users.length === 1 ? "s" : ""} it and will show black.` : "";
  if (!confirm(`Delete "${item.name}" and its conversions?${warn}`)) return;
  if (await act(() => api.del(`/api/v1/library/${encodeURIComponent(item.id)}`))) refreshLists();
}
const playTarget = computed(() => selectedOutput.value);
</script>

<template>
  <div class="panel">
    <h3>Upload</h3>
    <div class="drop" :class="{ over: dragging }" @dragover.prevent="dragging = true" @dragleave="dragging = false" @drop.prevent="dragging = false; onFiles($event.dataTransfer.files)">
      Drop videos, stills (PNG with alpha for keys) or animated GIFs here, or
      <input ref="fileInput" type="file" multiple aria-label="Files to upload" @change="onFiles($event.target.files)" />
    </div>
    <div class="row tight" style="margin-top: 0.6rem">
      <span class="muted small">aspect</span>
      <Segmented v-model="uploads.options.fit" :options="FITS" label="Aspect" class="small" />
      <span class="muted small">frame rate</span>
      <Segmented v-model="uploads.options.fps_mode" :options="FPS_MODES" label="Frame rate conversion" class="small" />
      <span class="muted small">alpha</span>
      <Segmented v-model="uploads.options.alpha_mode" :options="ALPHA_MODES" label="Alpha" class="small" />
    </div>
    <div class="row tight">
      <label class="check"><input v-model="uploads.options.loudness" type="checkbox" /> Loudness to −23 LUFS</label>
      <label for="up-xf" style="margin: 0">loop crossfade ms</label>
      <input id="up-xf" v-model.number="uploads.options.crossfade_ms" type="number" min="0" max="1000" style="width: 5.5rem" />
      <label for="up-ch" style="margin: 0" title="0 keeps the source channels">audio channels</label>
      <input id="up-ch" v-model.number="uploads.options.map_channels" type="number" min="0" max="64" style="width: 5rem" />
      <input v-model="uploads.options.tags" class="grow" placeholder="tags, comma separated" aria-label="Tags" />
    </div>
    <div v-if="uploads.files.length" class="uploads">
      <div v-for="(u, i) in uploads.files.slice(0, 6)" :key="i" class="upload">
        <strong>{{ u.name }}</strong> <span class="muted">{{ fmtBytes(u.size) }}</span> ·
        <span :class="{ 'msg err': u.state === 'failed' }">{{ uploadState(u) }}</span>
        <div v-if="u.state === 'sending'" class="progressbar"><div :style="{ width: `${(u.sent / Math.max(1, u.size)) * 100}%` }"></div></div>
      </div>
    </div>
  </div>

  <div class="panel">
    <h3>
      Conversions
      <span class="spacer"></span>
      <label class="check"><input v-model="showDone" type="checkbox" /> Show finished</label>
    </h3>
    <table v-if="jobs.length">
      <thead>
        <tr><th>Item</th><th>Format</th><th>State</th><th style="width: 30%">Progress</th><th class="num">Time</th><th>Error</th></tr>
      </thead>
      <tbody>
        <tr v-for="j in jobs" :key="j.id">
          <td>{{ itemsById[j.item_id]?.name || j.item_id }}</td>
          <td>{{ j.format }}</td>
          <td><Pill :text="j.state" :kind="{ done: 'ok', running: 'warn', failed: 'bad' }[j.state] || 'neutral'" /></td>
          <td><div class="progressbar" style="margin: 0"><div :style="{ width: `${(j.progress || 0) * 100}%` }"></div></div></td>
          <td class="num">{{ j.duration_s ? fmtSeconds(j.duration_s) : "–" }}</td>
          <td class="small" :class="{ 'msg err': j.error }">{{ j.error || "" }}</td>
        </tr>
      </tbody>
    </table>
    <div v-else class="empty">No conversion {{ showDone ? "yet" : "running" }}.</div>
  </div>

  <div class="panel">
    <h3>Items <span class="muted">{{ rows.length }} of {{ live.library.length }}</span></h3>
    <div class="row tight" style="margin-bottom: 0.6rem">
      <input v-model="query" type="search" class="grow" placeholder="Search name or tag" aria-label="Search the library" />
      <Segmented v-model="type" :options="TYPES" label="Type" />
    </div>
    <table v-if="rows.length">
      <thead>
        <tr><th></th><th>Name</th><th>Original</th><th>Conversions</th><th>Options</th><th>Used by</th><th></th></tr>
      </thead>
      <tbody>
        <tr v-for="item in rows" :key="item.id">
          <td class="thumb-cell">
            <img :src="`/api/v1/library/${item.id}/thumbnail?c=${Object.values(item.conversions || {}).filter((c) => c.status === 'ready').length}`" alt="" loading="lazy" @error="$event.target.style.visibility = 'hidden'" />
          </td>
          <td>
            <strong>{{ item.name }}</strong> <span class="badge">{{ item.type }}</span>
            <div class="tags" style="margin-top: 0.2rem"><span v-for="t in item.tags" :key="t" class="tag">{{ t }}</span></div>
          </td>
          <td class="small">{{ original(item) }}</td>
          <td>
            <span v-for="(c, f) in item.conversions" :key="f" class="pill" :class="STATUS_KIND[c.status] || 'neutral'" :title="c.error || ''">{{ f }} {{ c.status }}</span>
          </td>
          <td class="small muted">{{ item.fit }} · {{ item.fps_mode }}<span v-if="item.type === 'still'"> · {{ item.alpha_mode }}</span><span v-if="item.loudness"> · −23 LUFS</span></td>
          <td class="small">{{ usedBy(item).map((o) => o.label).join(", ") || "–" }}</td>
          <td class="actions-cell">
            <button v-if="item.type !== 'sprite' && playTarget" class="btn small" @click="playItem(playTarget.index, item)">Play on {{ outputLabel(playTarget.index) }}</button>
            <button class="btn small secondary" @click="openEdit(item, 'edit')">Edit</button>
            <button class="btn small secondary" @click="openEdit(item, 'reconvert')">Re-convert</button>
            <button class="btn small danger" @click="remove(item)">Delete</button>
          </td>
        </tr>
      </tbody>
    </table>
    <div v-else class="empty">{{ live.library.length ? "No item matches." : "The library is empty: upload a file above." }}</div>
    <p class="note">Play uses the output selected on the Outputs, Source or Burn-ins tab. Sprites (animated images) are moving-box content on the Burn-ins tab.</p>
  </div>

  <dialog ref="dialog" @close="editing = null">
    <template v-if="editing">
      <h3>{{ editing.mode === "edit" ? "Edit" : "Re-convert" }} {{ editing.item.name }}</h3>
      <template v-if="editing.mode === 'edit'">
        <label for="ed-name">Name</label>
        <input id="ed-name" v-model="editing.name" />
        <label for="ed-tags">Tags (comma separated)</label>
        <input id="ed-tags" v-model="editing.tags" />
      </template>
      <template v-else>
        <p class="note">The original upload is converted again for the player's format with these options.</p>
        <label>Aspect</label>
        <Segmented v-model="editing.options.fit" :options="FITS" label="Aspect" class="small" />
        <label>Frame rate</label>
        <Segmented v-model="editing.options.fps_mode" :options="FPS_MODES" label="Frame rate" class="small" />
        <label>Alpha</label>
        <Segmented v-model="editing.options.alpha_mode" :options="ALPHA_MODES" label="Alpha" class="small" />
        <label class="check"><input v-model="editing.options.loudness" type="checkbox" /> Loudness to −23 LUFS</label>
        <div class="row">
          <div><label for="ed-xf">Loop crossfade (ms)</label><input id="ed-xf" v-model.number="editing.options.crossfade_ms" type="number" min="0" max="1000" /></div>
          <div><label for="ed-ch">Audio channels (0 keeps)</label><input id="ed-ch" v-model.number="editing.options.map_channels" type="number" min="0" max="64" /></div>
        </div>
      </template>
      <div class="actions">
        <button class="btn secondary" @click="dialog.close()">Cancel</button>
        <button class="btn" :disabled="editing.mode === 'edit' && !editing.name.trim()" @click="saveEdit">{{ editing.mode === "edit" ? "Save" : "Re-convert" }}</button>
      </div>
    </template>
  </dialog>
</template>
