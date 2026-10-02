<script setup>
import { computed, onMounted, onUnmounted, ref } from "vue";

const page = ref("outputs");
const outputs = ref([]);
const library = ref([]);
const jobs = ref([]);
const presets = ref([]);
const playlists = ref([]);
const nmos = ref({});
const config = ref({});
const query = ref("");
const peaks = ref({});
const wsState = ref("connecting");
let ws;
let poll;
let thumbTimer;

async function api(path, opts = {}) {
  const res = await fetch(path, {
    headers: opts.body && !(opts.body instanceof ArrayBuffer) && typeof opts.body === "string" ? { "Content-Type": "application/json" } : {},
    ...opts,
  });
  if (res.status === 204) return null;
  const text = await res.text();
  const data = text ? JSON.parse(text) : null;
  if (!res.ok) throw new Error((data && data.error) || res.statusText);
  return data;
}

async function refresh() {
  if (page.value === "outputs" || page.value === "burnins") outputs.value = await api("/api/v1/outputs");
  if (page.value === "library" || page.value === "burnins" || page.value === "playlists") {
    library.value = await api("/api/v1/library?q=" + encodeURIComponent(query.value));
    jobs.value = await api("/api/v1/jobs");
  }
  if (page.value === "patterns") presets.value = await api("/api/v1/presets");
  if (page.value === "playlists") playlists.value = await api("/api/v1/playlists");
  if (page.value === "nmos" || page.value === "settings") {
    nmos.value = await api("/api/v1/nmos");
    config.value = await api("/api/v1/config");
  }
}

function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/api/v1/events`);
  ws.onopen = () => (wsState.value = "live");
  ws.onclose = () => {
    wsState.value = "reconnecting";
    setTimeout(connect, 1000);
  };
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (!msg.outputs) return;
    for (const o of msg.outputs) peaks.value[o.index] = o.peaks || [];
    if (page.value === "outputs" || page.value === "burnins") {
      outputs.value = msg.outputs.map((o) => o.status);
    }
    if (msg.jobs) jobs.value = msg.jobs;
  };
}

function meterHeight(v) {
  const db = v > 0 ? 20 * Math.log10(v) : -80;
  return Math.max(2, Math.min(48, ((db + 60) / 60) * 48));
}

async function transport(index, action) {
  await api(`/api/v1/outputs/${index}/transport`, { method: "POST", body: JSON.stringify({ action }) });
}
async function applyPreset(index, id) {
  await api(`/api/v1/outputs/${index}/source`, { method: "PUT", body: JSON.stringify({ preset: id }) });
}
async function patchOutput(index, body) {
  await api(`/api/v1/outputs/${index}`, { method: "PATCH", body: JSON.stringify(body) });
}

let textTimers = {};
function editText(output, layerIndex, text) {
  const key = output.index + ":" + layerIndex;
  clearTimeout(textTimers[key]);
  textTimers[key] = setTimeout(async () => {
    const burnin = JSON.parse(JSON.stringify(output.burnin));
    burnin.texts[layerIndex].text = text;
    await api(`/api/v1/outputs/${output.index}/burnin`, { method: "PUT", body: JSON.stringify(burnin) });
  }, 80);
}

async function toggleBurn(output, key, value) {
  const burnin = JSON.parse(JSON.stringify(output.burnin));
  burnin[key] = value;
  await api(`/api/v1/outputs/${output.index}/burnin`, { method: "PUT", body: JSON.stringify(burnin) });
}

async function addText(output) {
  const burnin = JSON.parse(JSON.stringify(output.burnin));
  burnin.texts = burnin.texts || [];
  if (burnin.texts.length >= 8) return;
  burnin.texts.push({ text: "Live {timecode}", anchor: "mc", size: 5, font: "DejaVu Sans", color: "#FFFFFF", opacity: 1, box: true, box_color: "#000000", box_opacity: 0.45, box_padding: 0.4, outline: false, shadow: true, offset_x: 0, offset_y: 0 });
  await api(`/api/v1/outputs/${output.index}/burnin`, { method: "PUT", body: JSON.stringify(burnin) });
}

async function setBox(output, box) {
  const burnin = JSON.parse(JSON.stringify(output.burnin));
  burnin.boxes = burnin.boxes && burnin.boxes.length ? burnin.boxes : [{ enabled: false, content: "builtin", path: "bounce", speed: 0.15, size: 0.12, opacity: 1, color: "#FFCC00", include_in_key: false, start_x: 0, start_y: 0 }];
  Object.assign(burnin.boxes[0], box);
  await api(`/api/v1/outputs/${output.index}/burnin`, { method: "PUT", body: JSON.stringify(burnin) });
}

const dragging = ref(false);
async function onFiles(fileList) {
  for (const file of fileList) {
    const meta = await api("/api/v1/uploads", { method: "POST", body: JSON.stringify({ name: file.name, size: file.size, options: { fit: "fit" } }) });
    const chunk = meta.chunk_size || 8 * 1024 * 1024;
    let n = 0;
    for (let off = 0; off < file.size; off += chunk, n++) {
      const slice = file.slice(off, Math.min(file.size, off + chunk));
      const buf = await slice.arrayBuffer();
      await fetch(`/api/v1/uploads/${meta.id}/chunks/${n}`, { method: "PUT", body: buf });
    }
    await api(`/api/v1/uploads/${meta.id}/complete`, { method: "POST", body: "{}" });
  }
  await refresh();
}

async function playItem(outputIndex, item) {
  const type = item.type === "still" ? "still" : item.type === "sprite" ? "still" : "video";
  await api(`/api/v1/outputs/${outputIndex}/source`, { method: "PUT", body: JSON.stringify({ type, item_id: item.id, audio: item.type === "still" ? "sine" : "sine" }) });
  await transport(outputIndex, "play");
}

const newPreset = ref({ name: "Custom", video: { pattern: "bars_75", pluge: false }, audio: { signal: "ident_freq", level_dbfs: -18, frequency: 1000 } });
async function savePreset() {
  await api("/api/v1/presets", { method: "POST", body: JSON.stringify(newPreset.value) });
  presets.value = await api("/api/v1/presets");
}

const newPl = ref({ name: "Playlist", entries: [{ item_id: "", loops: 1 }] });
async function savePlaylist() {
  await api("/api/v1/playlists", { method: "POST", body: JSON.stringify(newPl.value) });
  playlists.value = await api("/api/v1/playlists");
}
async function usePlaylist(outputIndex, pl) {
  const entries = [];
  for (const e of pl.entries || []) {
    const item = library.value.find((i) => i.id === e.item_id) || (await api("/api/v1/library")).find((i) => i.id === e.item_id);
    const frames = item && item.conversions ? Object.values(item.conversions)[0]?.frames || 1 : 1;
    entries.push({ item_id: e.item_id, loops: e.loops, frames });
  }
  await api(`/api/v1/outputs/${outputIndex}/source`, { method: "PUT", body: JSON.stringify({ type: "playlist", playlist_id: pl.id, entries, item_id: entries[0]?.item_id || "" }) });
}

const filtered = computed(() => library.value);

onMounted(async () => {
  presets.value = await api("/api/v1/presets");
  await refresh();
  connect();
  poll = setInterval(refresh, 2000);
  thumbTimer = setInterval(() => {
    document.querySelectorAll("img.thumb").forEach((img) => {
      const src = img.getAttribute("data-src");
      if (src) img.src = src + "?t=" + Date.now();
    });
  }, 500);
});
onUnmounted(() => {
  clearInterval(poll);
  clearInterval(thumbTimer);
  if (ws) ws.close();
});
</script>

<template>
  <header class="app">
    <h1>MXL Test Player</h1>
    <nav>
      <button v-for="p in ['outputs', 'burnins', 'library', 'patterns', 'playlists', 'nmos', 'settings']" :key="p" :class="{ active: page === p }" @click="page = p; refresh()">{{ p }}</button>
    </nav>
    <span class="muted">events {{ wsState }}</span>
  </header>
  <main>
    <section v-if="page === 'outputs'" class="grid">
      <article v-for="o in outputs" :key="o.index" class="card">
        <h2>{{ o.label }} · {{ o.format }} · {{ o.transport }}</h2>
        <img class="thumb" :data-src="`/api/v1/outputs/${o.index}/thumbnail`" alt="thumbnail" />
        <div class="row" style="margin-top: 0.5rem">
          <button class="primary" @click="transport(o.index, 'play')">Play</button>
          <button @click="transport(o.index, 'pause')">Pause</button>
          <button @click="transport(o.index, 'stop')">Stop</button>
          <button @click="transport(o.index, 'restart')">Restart</button>
          <button @click="transport(o.index, 'step')">Step</button>
          <label class="chk"><input type="checkbox" :checked="o.loop" @change="patchOutput(o.index, { loop: $event.target.checked })" /> loop</label>
        </div>
        <div class="progress" style="margin: 0.5rem 0"><i :style="{ width: (o.progress * 100) + '%' }"></i></div>
        <div class="muted">grains {{ o.grains }} · underruns {{ o.underruns }} · loops {{ o.loops }} · flow {{ o.video_flow_id.slice(0, 8) }}</div>
        <div class="meters" style="margin-top: 0.4rem">
          <span v-for="(p, i) in (peaks[o.index] || [])" :key="i" :style="{ height: meterHeight(p) + 'px' }"></span>
        </div>
        <div class="row" style="margin-top: 0.5rem">
          <select @change="applyPreset(o.index, $event.target.value)">
            <option value="">preset…</option>
            <option v-for="p in presets" :key="p.id" :value="p.id">{{ p.name }}</option>
          </select>
          <select :value="o.key_mode" @change="patchOutput(o.index, { key_mode: $event.target.value })">
            <option value="off">key off</option>
            <option value="fill_key">fill + key</option>
            <option value="v210a">v210a</option>
          </select>
        </div>
      </article>
    </section>

    <section v-else-if="page === 'burnins'" class="grid">
      <article v-for="o in outputs" :key="o.index" class="card">
        <h2>{{ o.label }} burn-ins</h2>
        <div class="row">
          <label class="chk" v-for="k in ['label', 'timecode', 'utc_clock', 'local_clock', 'frame_counter', 'item', 'flow_id', 'on_keyed_stills']" :key="k">
            <input type="checkbox" :checked="o.burnin[k]" @change="toggleBurn(o, k, $event.target.checked)" /> {{ k }}
          </label>
        </div>
        <div v-for="(layer, i) in o.burnin.texts" :key="i" class="layer">
          <textarea :value="layer.text" @input="editText(o, i, $event.target.value)"></textarea>
          <div class="row">
            <select :value="layer.anchor" @change="layer.anchor = $event.target.value; editText(o, i, layer.text)">
              <option v-for="a in ['tl','tc','tr','ml','mc','mr','bl','bc','br']" :key="a">{{ a }}</option>
            </select>
            <input :value="layer.size" type="number" step="0.5" @change="layer.size = Number($event.target.value); editText(o, i, layer.text)" />
            <select :value="layer.font" @change="layer.font = $event.target.value; editText(o, i, layer.text)">
              <option>DejaVu Sans</option>
              <option>DejaVu Sans Bold</option>
              <option>DejaVu Sans Mono</option>
            </select>
            <input :value="layer.color" @change="layer.color = $event.target.value; editText(o, i, layer.text)" />
          </div>
        </div>
        <button @click="addText(o)">Add text layer</button>
        <div class="layer">
          <strong>Moving box</strong>
          <div class="row">
            <label class="chk"><input type="checkbox" :checked="o.burnin.boxes?.[0]?.enabled" @change="setBox(o, { enabled: $event.target.checked })" /> on</label>
            <select @change="setBox(o, { path: $event.target.value })">
              <option v-for="p in ['bounce','horizontal','vertical','circle','diagonal']" :key="p">{{ p }}</option>
            </select>
            <input type="number" step="0.05" placeholder="speed" @change="setBox(o, { speed: Number($event.target.value) })" />
            <input type="number" step="0.02" placeholder="size" @change="setBox(o, { size: Number($event.target.value) })" />
            <select @change="setBox(o, { content: $event.target.value })">
              <option value="builtin">built-in box</option>
              <option v-for="it in library.filter(i => i.type === 'sprite' || i.type === 'still')" :key="it.id" :value="it.id">{{ it.name }}</option>
            </select>
          </div>
        </div>
      </article>
    </section>

    <section v-else-if="page === 'library'">
      <div class="drop" :class="{ over: dragging }" @dragover.prevent="dragging = true" @dragleave="dragging = false" @drop.prevent="dragging = false; onFiles($event.dataTransfer.files)">
        Drop media here or <input type="file" multiple @change="onFiles($event.target.files)" />
      </div>
      <div class="row" style="margin: 0.6rem 0">
        <input v-model="query" placeholder="search" @change="refresh" />
      </div>
      <table>
        <thead><tr><th>name</th><th>type</th><th>status</th><th></th></tr></thead>
        <tbody>
          <tr v-for="it in filtered" :key="it.id">
            <td>{{ it.name }}<div class="muted">{{ it.original?.width }}×{{ it.original?.height }} {{ it.original?.codec }}</div></td>
            <td>{{ it.type }}</td>
            <td>{{ Object.entries(it.conversions || {}).map(([k,v]) => k + ':' + v.status).join(' ') }}</td>
            <td class="row">
              <button v-for="o in outputs" :key="o.index" @click="playItem(o.index, it)">to {{ o.label }}</button>
              <button class="danger" @click="api('/api/v1/library/' + it.id, { method: 'DELETE' }).then(refresh)">delete</button>
            </td>
          </tr>
        </tbody>
      </table>
      <h3>Jobs</h3>
      <div v-for="j in jobs" :key="j.id" class="muted">{{ j.state }} {{ j.format }} {{ j.item_id }} {{ j.error || '' }}</div>
    </section>

    <section v-else-if="page === 'patterns'" class="card">
      <h2>Presets</h2>
      <table>
        <tbody>
          <tr v-for="p in presets" :key="p.id">
            <td>{{ p.name }}</td>
            <td class="muted">{{ p.video?.pattern }} + {{ p.audio?.signal }}</td>
            <td><button v-if="!p.builtin" class="danger" @click="api('/api/v1/presets/' + p.id, { method: 'DELETE' }).then(refresh)">delete</button></td>
          </tr>
        </tbody>
      </table>
      <div class="row" style="margin-top: 0.6rem">
        <input v-model="newPreset.name" />
        <input v-model="newPreset.video.pattern" />
        <input v-model="newPreset.audio.signal" />
        <button class="primary" @click="savePreset">Save preset</button>
      </div>
    </section>

    <section v-else-if="page === 'playlists'" class="card">
      <h2>Playlists</h2>
      <div v-for="pl in playlists" :key="pl.id" class="layer">
        <strong>{{ pl.name }}</strong>
        <div class="muted">{{ (pl.entries || []).map(e => e.item_id + ' x' + e.loops).join(', ') }}</div>
        <button v-for="o in outputs" :key="o.index" @click="usePlaylist(o.index, pl)">play on {{ o.label }}</button>
      </div>
      <div class="row">
        <input v-model="newPl.name" />
        <input v-model="newPl.entries[0].item_id" placeholder="item id" />
        <input v-model.number="newPl.entries[0].loops" type="number" />
        <button class="primary" @click="savePlaylist">Create</button>
      </div>
    </section>

    <section v-else-if="page === 'nmos'" class="card">
      <h2>NMOS</h2>
      <pre>{{ JSON.stringify(nmos, null, 2) }}</pre>
      <p class="muted">Node API is on the NMOS port. Senders use urn:x-nmos:transport:mxl. DNS-SD stays off unless NMOS_DNS_SD is set.</p>
    </section>

    <section v-else class="card">
      <h2>Settings</h2>
      <pre>{{ JSON.stringify(config, null, 2) }}</pre>
    </section>
  </main>
</template>
