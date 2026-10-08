<script setup>
// Playlists (§7, §9): ordered library items, each with its loop count (0 on the last item: loop
// it forever) and, for a still, its hold time. Cut transitions. Play on the selected output.
import { computed, ref, watch } from "vue";
import OutputPicker from "./OutputPicker.vue";
import { api, canon, clone, fmtSeconds, formatFps } from "../api.js";
import { act, drafts, itemFrames, itemsById, live, playPlaylist, refreshLists, selectedOutput } from "../store.js";

// The draft lives in the store: it survives tab switches and reconnects.
const d = computed(() => drafts.playlist);
const dirty = computed(() => !!d.value && canon(d.value.value) !== d.value.saved);
const newName = ref("");
const addItem = ref("");

function load(id) {
  if (d.value && id !== d.value.id && dirty.value && !confirm(`Discard the unsaved changes to "${d.value.value.name}"?`)) return;
  const pl = live.playlists.find((p) => p.id === id);
  if (!pl) return;
  const value = { name: pl.name || "", entries: (pl.entries || []).map((e) => ({ item_id: e.item_id, loops: Number(e.loops ?? 1), hold_s: Number(e.hold_s ?? 5) })) };
  drafts.playlist = { id, value, saved: canon(value) };
}
watch(
  () => live.playlists.length,
  () => {
    if ((!d.value || !live.playlists.some((p) => p.id === d.value.id)) && live.playlists.length) load(live.playlists[0].id);
  },
  { immediate: true },
);

const fps = computed(() => formatFps(selectedOutput.value?.format));
function seconds(e) {
  const item = itemsById.value[e.item_id];
  if (!item) return 0;
  if (item.type === "still") return Number(e.hold_s) || 0;
  return itemFrames(item, selectedOutput.value?.format) / fps.value;
}
const total = computed(() => (d.value?.value.entries || []).reduce((sum, e) => sum + seconds(e) * (Number(e.loops) || 1), 0));
const endless = computed(() => (d.value?.value.entries || []).some((e) => Number(e.loops) === 0));
const playable = computed(() => live.library.filter((i) => i.type === "video" || i.type === "still"));
const playingOn = (id) => live.outputs.filter((o) => o.source?.type === "playlist" && o.source.playlist_id === id);

function move(i, by) {
  const list = d.value.value.entries;
  const [e] = list.splice(i, 1);
  list.splice(i + by, 0, e);
}
function add() {
  if (!addItem.value) return;
  d.value.value.entries.push({ item_id: addItem.value, loops: 1, hold_s: 5 });
  addItem.value = "";
}
async function create() {
  const created = await act(() => api.post("/api/v1/playlists", { name: newName.value.trim(), entries: [] }));
  if (!created) return;
  newName.value = "";
  await refreshLists();
  load(created.id);
}
async function save() {
  const body = clone(d.value.value);
  body.name = body.name.trim();
  if (await act(() => api.put(`/api/v1/playlists/${encodeURIComponent(d.value.id)}`, body), `Saved playlist ${body.name}.`)) {
    d.value.saved = canon(d.value.value);
    refreshLists();
  }
}
async function remove() {
  if (!confirm(`Delete playlist "${d.value.value.name}"? The items stay in the library.`)) return;
  if (await act(() => api.del(`/api/v1/playlists/${encodeURIComponent(d.value.id)}`))) {
    drafts.playlist = null;
    await refreshLists();
    if (live.playlists.length) load(live.playlists[0].id);
  }
}
function revert() {
  load(d.value.id); // the saved playlist, as the player has it now
}
async function play() {
  const pl = live.playlists.find((p) => p.id === d.value.id);
  if (pl) await playPlaylist(selectedOutput.value.index, pl);
}
</script>

<template>
  <div class="toolbar">
    <OutputPicker />
    <span class="muted small">Play sends the playlist to this output.</span>
  </div>
  <div class="split">
    <div class="panel">
      <h3>Playlists</h3>
      <button v-for="p in live.playlists" :key="p.id" class="listbtn" :class="{ active: p.id === d?.id }" :aria-pressed="p.id === d?.id" @click="load(p.id)">
        <strong>{{ p.name || "(no name)" }}</strong>
        <span v-for="o in playingOn(p.id)" :key="o.index" class="pill ok" style="margin-left: 0.4rem">{{ o.label }}</span>
        <div class="meta">{{ (p.entries || []).length }} items</div>
      </button>
      <div v-if="!live.playlists.length" class="empty">No playlists yet.</div>
      <form class="row tight" style="margin-top: 0.8rem" @submit.prevent="create">
        <input v-model="newName" class="grow" placeholder="New playlist name" aria-label="New playlist name" />
        <button class="btn" type="submit" :disabled="!newName.trim()">Create</button>
      </form>
    </div>

    <div v-if="d" class="panel">
      <h3>
        Edit playlist
        <span class="spacer"></span>
        <span v-if="dirty" class="pill warn">not saved</span>
        <span v-for="o in playingOn(d.id)" :key="o.index" class="pill ok">{{ o.label }} {{ o.transport }}</span>
      </h3>
      <label for="pl-name">Name</label>
      <input id="pl-name" v-model="d.value.name" maxlength="120" />
      <table style="margin-top: 0.8rem">
        <thead>
          <tr>
            <th class="num">#</th>
            <th>Item</th>
            <th class="num">Length</th>
            <th title="how often it plays; 0 on the last item loops it forever">Loops</th>
            <th title="how long a still stays on air">Hold (s)</th>
            <th></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="(e, i) in d.value.entries" :key="i" :class="{ dim: !itemsById[e.item_id] }">
            <td class="num">{{ i + 1 }}</td>
            <td>
              <template v-if="itemsById[e.item_id]">
                {{ itemsById[e.item_id].name }} <span class="badge">{{ itemsById[e.item_id].type }}</span>
              </template>
              <span v-else class="muted">{{ e.item_id }} (deleted, skipped)</span>
            </td>
            <td class="num">{{ itemsById[e.item_id] ? fmtSeconds(seconds(e)) : "–" }}</td>
            <td><input v-model.number="e.loops" type="number" min="0" step="1" style="width: 5rem" :class="{ invalid: !(e.loops >= 0) }" :aria-label="`Loops of item ${i + 1}`" /></td>
            <td>
              <input v-if="itemsById[e.item_id]?.type === 'still'" v-model.number="e.hold_s" type="number" min="0.1" step="0.5" style="width: 5rem" :aria-label="`Hold time of item ${i + 1}`" />
              <span v-else class="muted">–</span>
            </td>
            <td class="actions-cell">
              <button class="btn small secondary" :disabled="i === 0" title="Move up" @click="move(i, -1)">↑</button>
              <button class="btn small secondary" :disabled="i === d.value.entries.length - 1" title="Move down" @click="move(i, 1)">↓</button>
              <button class="btn small danger" @click="d.value.entries.splice(i, 1)">Remove</button>
            </td>
          </tr>
        </tbody>
      </table>
      <div v-if="!d.value.entries.length" class="empty">No items yet: add some below.</div>
      <div class="row tight" style="margin-top: 0.7rem">
        <select v-model="addItem" class="grow" aria-label="Item to add">
          <option value="">Add an item…</option>
          <option v-for="i in playable" :key="i.id" :value="i.id">{{ i.name }} ({{ i.type }})</option>
        </select>
        <button class="btn secondary" :disabled="!addItem" @click="add">Add</button>
      </div>
      <p class="note">
        Total {{ endless ? "endless (an item loops forever)" : fmtSeconds(total) }} at {{ selectedOutput?.format }}. Items play with cuts; the next item is loaded ahead.
      </p>
      <div class="actions">
        <button class="btn danger" @click="remove">Delete</button>
        <span class="spacer"></span>
        <button class="btn secondary" :disabled="!dirty" @click="revert">Revert</button>
        <button class="btn" :disabled="!dirty || !d.value.name.trim()" @click="save">Save</button>
        <button class="btn" :disabled="dirty || !d.value.entries.length || !selectedOutput" :title="dirty ? 'save first' : ''" @click="play">Play on {{ selectedOutput?.label }}</button>
      </div>
    </div>
    <div v-else class="panel empty">Create a playlist on the left.</div>
  </div>
</template>
