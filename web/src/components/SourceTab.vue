<script setup>
// Source and output settings of the selected output (§5, §7, §7.1, §7.2). Both are drafts:
// Apply sends them, Revert takes the output's values again. A source change keeps the flows;
// label, format, audio channels, key mode and the ANC flow open the flows again.
import { computed, ref, watch } from "vue";
import LivePicture from "./LivePicture.vue";
import Meters from "./Meters.vue";
import OutputPicker from "./OutputPicker.vue";
import Segmented from "./Segmented.vue";
import Transport from "./Transport.vue";
import { api, ATC_KINDS, FORMATS, IDLE_KEYS, KEY_MODES, PATTERN_GROUPS, PAUSE_AUDIO, PLUGE_PATTERNS, SIGNALS, TC_SOURCES } from "../api.js";
import {
  act,
  drafts,
  isDirty,
  live,
  patchOutput,
  playPlaylist,
  refreshLists,
  revertDraft,
  selectedOutput,
  setSource,
  setupOf,
  sourceOf,
  syncDraft,
} from "../store.js";

const o = selectedOutput;
watch(
  () => o.value && [o.value.index, sourceOf(o.value), setupOf(o.value)],
  () => {
    if (!o.value) return;
    syncDraft("source", o.value.index, sourceOf(o.value));
    syncDraft("setup", o.value.index, setupOf(o.value));
  },
  { immediate: true, deep: true },
);
const src = computed(() => drafts.source[o.value?.index]?.value);
const setup = computed(() => drafts.setup[o.value?.index]?.value);
const srcDirty = computed(() => o.value && isDirty("source", o.value.index));
const setupDirty = computed(() => o.value && isDirty("setup", o.value.index));

const TYPES = [
  { value: "pattern", label: "Test pattern" },
  { value: "video", label: "Video" },
  { value: "still", label: "Still" },
  { value: "playlist", label: "Playlist" },
];
const signal = computed(() => SIGNALS.find((s) => s.value === src.value?.audio) || SIGNALS[0]);
const items = (type) => live.library.filter((i) => i.type === type);
const ready = (item) => item.conversions?.[o.value?.format]?.status === "ready";
const hasAudioChoice = computed(() => src.value && (src.value.type === "pattern" || src.value.type === "still"));

async function applySource() {
  const s = src.value;
  const index = o.value.index;
  if (s.type === "playlist") {
    const pl = live.playlists.find((p) => p.id === s.playlist_id);
    if (pl && (await playPlaylist(index, pl))) revertDraft("source", index, sourceOf(live.outputs.find((x) => x.index === index)));
    return;
  }
  const body = { ...s, frequency: Number(s.frequency), level_dbfs: Number(s.level_dbfs) };
  if (s.type === "pattern") body.item_id = "";
  if (await setSource(index, body)) revertDraft("source", index, sourceOf(live.outputs.find((x) => x.index === index)));
}
const canApply = computed(() => {
  const s = src.value;
  if (!s) return false;
  if (s.type === "video" || s.type === "still") return !!s.item_id;
  if (s.type === "playlist") return !!s.playlist_id;
  return true;
});

// Save the pattern draft as a preset (§5.3).
const presetName = ref("");
async function savePreset() {
  const s = src.value;
  const body = {
    name: presetName.value.trim(),
    video: { pattern: s.pattern, pluge: s.pluge },
    audio: { signal: s.audio, frequency: Number(s.frequency), level_dbfs: Number(s.level_dbfs) },
  };
  if (await act(() => api.post("/api/v1/presets", body), `Saved preset ${body.name}.`)) {
    presetName.value = "";
    refreshLists();
  }
}

// Output settings: only the changed fields go out (a label, even unchanged, reopens the flows).
const setupChanges = computed(() => {
  if (!setup.value || !o.value) return {};
  const base = setupOf(o.value);
  return Object.fromEntries(Object.entries(setup.value).filter(([k, v]) => v !== base[k]));
});
const REOPEN = ["label", "format", "audio_channels", "key_mode", "anc"];
const reopens = computed(() => Object.keys(setupChanges.value).some((k) => REOPEN.includes(k)));
const newIds = computed(() => ["format", "audio_channels"].some((k) => k in setupChanges.value) || ("key_mode" in setupChanges.value && [setup.value.key_mode, o.value.key_mode].includes("v210a")));
const dropFrameApplies = computed(() => /(29\.97|59\.94)$/.test(setup.value?.format || ""));
const channelsOk = computed(() => Number.isInteger(Number(setup.value?.audio_channels)) && setup.value.audio_channels >= 2 && setup.value.audio_channels <= 64);
async function applySetup() {
  const body = { ...setupChanges.value };
  if ("audio_channels" in body) body.audio_channels = Number(body.audio_channels);
  const index = o.value.index;
  if (await patchOutput(index, body)) revertDraft("setup", index, setupOf(live.outputs.find((x) => x.index === index)));
}
const dirtyField = (k) => (k in setupChanges.value ? "dirty" : "");
</script>

<template>
  <div class="toolbar"><OutputPicker /></div>
  <div v-if="!o || !src || !setup" class="empty">Waiting for the player…</div>
  <div v-else class="editor">
    <div class="main">
      <div class="panel">
        <h3>
          Source
          <span class="spacer"></span>
          <span v-if="srcDirty" class="pill warn">not applied</span>
        </h3>
        <Segmented v-model="src.type" :options="TYPES" label="Source type" />

        <template v-if="src.type === 'pattern'">
          <div v-for="g in PATTERN_GROUPS" :key="g.label">
            <div class="group-caption">{{ g.label }}</div>
            <Segmented v-model="src.pattern" :options="g.items" :label="g.label" class="small" />
          </div>
          <label class="check" :title="PLUGE_PATTERNS.includes(src.pattern) ? '' : 'bars only'">
            <input v-model="src.pluge" type="checkbox" :disabled="!PLUGE_PATTERNS.includes(src.pattern)" /> PLUGE under the bars
          </label>
        </template>

        <template v-else-if="src.type === 'video' || src.type === 'still'">
          <label for="src-item">{{ src.type === "video" ? "Video" : "Still image" }} from the library</label>
          <select id="src-item" v-model="src.item_id" :class="{ invalid: !src.item_id }">
            <option value="">Choose…</option>
            <option v-for="i in items(src.type)" :key="i.id" :value="i.id">{{ i.name }}{{ ready(i) ? "" : ` (not converted for ${o.format} yet)` }}</option>
          </select>
          <p v-if="!items(src.type).length" class="note">No {{ src.type }} items yet: upload one on the Library tab.</p>
          <p v-else-if="src.type === 'video'" class="note">A video plays its own audio, gapless in a loop when Loop is on.</p>
        </template>

        <template v-else>
          <label for="src-pl">Playlist</label>
          <select id="src-pl" v-model="src.playlist_id" :class="{ invalid: !src.playlist_id }">
            <option value="">Choose…</option>
            <option v-for="p in live.playlists" :key="p.id" :value="p.id">{{ p.name }} ({{ (p.entries || []).length }} items)</option>
          </select>
          <p class="note">Apply plays the playlist from its first item. Edit playlists on the Playlists tab.</p>
        </template>

        <template v-if="hasAudioChoice">
          <div class="group-caption">Audio</div>
          <Segmented v-model="src.audio" :options="SIGNALS" label="Audio signal" class="small" />
          <div class="fields grid" style="margin-top: 0.2rem">
            <div>
              <label for="src-freq">Frequency (Hz)</label>
              <input id="src-freq" v-model.number="src.frequency" type="number" min="20" max="20000" step="1" :disabled="!signal.freq" />
            </div>
            <div>
              <label for="src-level">Level (dBFS)</label>
              <input id="src-level" v-model.number="src.level_dbfs" type="number" min="-60" max="0" step="0.5" :disabled="src.audio === 'silence'" />
            </div>
          </div>
          <label class="check"><input v-model="src.sync_beep" type="checkbox" /> Sync beep once per second (with the A/V sync flash)</label>
        </template>

        <div class="actions">
          <template v-if="src.type === 'pattern'">
            <input v-model="presetName" placeholder="Preset name" aria-label="Preset name" style="width: 12rem" />
            <button class="btn secondary" :disabled="!presetName.trim()" @click="savePreset">Save as preset</button>
            <span class="spacer"></span>
          </template>
          <button class="btn secondary" :disabled="!srcDirty" @click="revertDraft('source', o.index, sourceOf(o))">Revert</button>
          <button class="btn" :disabled="!canApply" @click="applySource">Apply to {{ o.label }}</button>
        </div>
      </div>

      <div class="panel">
        <h3>
          Output
          <span class="spacer"></span>
          <span v-if="setupDirty" class="pill warn">not applied</span>
        </h3>
        <div class="fields grid">
          <div>
            <label for="set-label">Label</label>
            <input id="set-label" v-model="setup.label" :class="[dirtyField('label'), { invalid: !setup.label.trim() }]" />
          </div>
          <div>
            <label for="set-format">Format</label>
            <select id="set-format" v-model="setup.format" :class="dirtyField('format')">
              <option v-for="f in FORMATS" :key="f" :value="f">{{ f }}</option>
            </select>
          </div>
          <div>
            <label for="set-ch">Audio channels</label>
            <input id="set-ch" v-model.number="setup.audio_channels" type="number" min="2" max="64" :class="[dirtyField('audio_channels'), { invalid: !channelsOk }]" />
          </div>
        </div>
        <label>Key</label>
        <div class="row tight">
          <Segmented v-model="setup.key_mode" :options="KEY_MODES" label="Key mode" class="small" />
          <span class="muted small">idle key</span>
          <Segmented v-model="setup.idle_key" :options="IDLE_KEYS" label="Idle key" class="small" />
        </div>
        <label>Timecode</label>
        <div class="row tight">
          <label class="check"><input v-model="setup.anc" type="checkbox" /> ANC timecode flow</label>
          <Segmented v-model="setup.tc_source" :options="TC_SOURCES" label="Timecode source" class="small" />
        </div>
        <div class="row tight">
          <Segmented v-model="setup.atc" :options="ATC_KINDS" label="ATC kind" class="small" />
          <label class="check" :title="dropFrameApplies ? '' : 'only at 29.97 and 59.94'">
            <input v-model="setup.drop_frame" type="checkbox" :disabled="!dropFrameApplies" /> Drop frame
          </label>
        </div>
        <label>Playback</label>
        <div class="row tight">
          <span class="muted small">audio while paused</span>
          <Segmented v-model="setup.pause_audio" :options="PAUSE_AUDIO" label="Audio while paused" class="small" />
          <label class="check"><input v-model="setup.loop" type="checkbox" /> Loop videos</label>
        </div>
        <div v-if="reopens" class="warnbox">
          {{ newIds ? "The flows get new ids: receivers must be routed again." : "The flows are opened again (same ids)." }}
        </div>
        <p class="note">
          These apply at once. After a restart the output settings come from the configuration file again; the source, transport, burn-ins and
          sender enables are kept.
        </p>
        <div class="actions">
          <button class="btn secondary" :disabled="!setupDirty" @click="revertDraft('setup', o.index, setupOf(o))">Revert</button>
          <button class="btn" :disabled="!setupDirty || !channelsOk || !setup.label.trim()" @click="applySetup">Apply</button>
        </div>
      </div>
    </div>

    <div class="side">
      <div class="panel">
        <h3>
          {{ o.label }} <span class="fmt">{{ o.format }}</span>
          <span class="spacer"></span>
          <span class="state-tag" :class="o.transport">{{ o.transport.toUpperCase() }}</span>
        </h3>
        <LivePicture :src="`/api/v1/outputs/${o.index}/thumbnail`" :alt="o.label" style="margin-bottom: 0.6rem" />
        <Transport :output="o" />
        <Meters :peaks="live.peaks[o.index] || []" style="margin-top: 0.6rem" />
      </div>
    </div>
  </div>
</template>
