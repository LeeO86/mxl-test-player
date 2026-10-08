<script setup>
// Patterns (§5.3, §9): presets of video pattern and audio signal, built in and saved ones. Apply
// sends one to the selected output; the editor saves a new preset (the API has no preset edit:
// save the changed one as new and delete the old one).
import { computed } from "vue";
import OutputPicker from "./OutputPicker.vue";
import Segmented from "./Segmented.vue";
import { api, PATTERNS, PLUGE_PATTERNS, patternLabel, SIGNALS, signalLabel } from "../api.js";
import { act, drafts, live, refreshLists, selectedOutput, setSource } from "../store.js";

const EMPTY = { name: "", pattern: "smpte_rp219", pluge: false, signal: "sine", frequency: 1000, level_dbfs: -18 };
if (!drafts.preset) drafts.preset = { ...EMPTY };
const p = computed(() => drafts.preset);
const signal = computed(() => SIGNALS.find((s) => s.value === p.value.signal) || SIGNALS[0]);

function edit(preset) {
  drafts.preset = {
    name: preset.builtin ? `${preset.name} (copy)` : preset.name,
    pattern: preset.video?.pattern || "smpte_rp219",
    pluge: !!preset.video?.pluge,
    signal: preset.audio?.signal || "sine",
    frequency: preset.audio?.frequency ?? 1000,
    level_dbfs: preset.audio?.level_dbfs ?? -18,
  };
}
function fromOutput() {
  const s = selectedOutput.value?.source;
  if (s?.type !== "pattern") return;
  drafts.preset = { name: p.value.name, pattern: s.pattern, pluge: s.pluge, signal: s.audio, frequency: s.frequency, level_dbfs: s.level_dbfs };
}
async function save() {
  const d = p.value;
  const body = { name: d.name.trim(), video: { pattern: d.pattern, pluge: d.pluge }, audio: { signal: d.signal, frequency: Number(d.frequency), level_dbfs: Number(d.level_dbfs) } };
  if (await act(() => api.post("/api/v1/presets", body), `Saved preset ${body.name}.`)) {
    drafts.preset = { ...EMPTY };
    refreshLists();
  }
}
async function remove(preset) {
  if (!confirm(`Delete preset "${preset.name}"?`)) return;
  if (await act(() => api.del(`/api/v1/presets/${encodeURIComponent(preset.id)}`))) refreshLists();
}
const audioText = (a) => `${signalLabel(a?.signal)}${SIGNALS.find((s) => s.value === a?.signal)?.freq ? ` ${a.frequency ?? 1000} Hz` : ""}${a?.signal === "silence" ? "" : ` · ${a?.level_dbfs ?? -18} dBFS`}`;
</script>

<template>
  <div class="toolbar">
    <OutputPicker />
    <span class="muted small">Apply sends the preset to this output.</span>
  </div>
  <div class="panel">
    <h3>Presets</h3>
    <table>
      <thead>
        <tr><th>Name</th><th>Video</th><th>Audio</th><th></th></tr>
      </thead>
      <tbody>
        <tr v-for="x in live.presets" :key="x.id">
          <td><strong>{{ x.name }}</strong><span v-if="x.builtin" class="badge">built in</span></td>
          <td>{{ patternLabel(x.video?.pattern) }}<span v-if="x.video?.pluge"> + PLUGE</span></td>
          <td>{{ audioText(x.audio) }}</td>
          <td class="actions-cell">
            <button class="btn small" :disabled="!selectedOutput" @click="setSource(selectedOutput.index, { preset: x.id })">Apply to {{ selectedOutput?.label }}</button>
            <button class="btn small secondary" @click="edit(x)">{{ x.builtin ? "Copy" : "Edit" }}</button>
            <button v-if="!x.builtin" class="btn small danger" @click="remove(x)">Delete</button>
          </td>
        </tr>
      </tbody>
    </table>
  </div>

  <div class="panel">
    <h3>
      New preset
      <span class="spacer"></span>
      <button class="btn small secondary" :disabled="selectedOutput?.source?.type !== 'pattern'" @click="fromOutput">Take from {{ selectedOutput?.label }}</button>
    </h3>
    <div class="fields grid">
      <div>
        <label for="pr-name">Name</label>
        <input id="pr-name" v-model="p.name" placeholder="e.g. Bars + 997 Hz" :class="{ invalid: !p.name.trim() }" />
      </div>
      <div>
        <label for="pr-pattern">Video pattern</label>
        <select id="pr-pattern" v-model="p.pattern">
          <option v-for="x in PATTERNS" :key="x.value" :value="x.value">{{ x.label }}</option>
        </select>
      </div>
      <div>
        <label for="pr-freq">Frequency (Hz)</label>
        <input id="pr-freq" v-model.number="p.frequency" type="number" min="20" max="20000" :disabled="!signal.freq" />
      </div>
      <div>
        <label for="pr-level">Level (dBFS)</label>
        <input id="pr-level" v-model.number="p.level_dbfs" type="number" min="-60" max="0" step="0.5" :disabled="p.signal === 'silence'" />
      </div>
    </div>
    <label class="check"><input v-model="p.pluge" type="checkbox" :disabled="!PLUGE_PATTERNS.includes(p.pattern)" /> PLUGE under the bars</label>
    <label>Audio signal</label>
    <Segmented v-model="p.signal" :options="SIGNALS" label="Audio signal" class="small" />
    <p class="note">A preset with the A/V sync pattern or the sync beep flashes and beeps once per second.</p>
    <div class="actions">
      <button class="btn secondary" @click="drafts.preset = { ...EMPTY }">Clear</button>
      <button class="btn" :disabled="!p.name.trim()" @click="save">Save preset</button>
    </div>
  </div>
</template>
