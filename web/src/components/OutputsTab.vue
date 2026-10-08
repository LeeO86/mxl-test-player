<script setup>
// Outputs (§9): one card per output with its picture, source, transport, position, audio
// meters, a preset in one click, and the counters. A click on the picture selects the output
// for the Source and Burn-ins tabs.
import { inject } from "vue";
import LivePicture from "./LivePicture.vue";
import Meters from "./Meters.vue";
import Pill from "./Pill.vue";
import Transport from "./Transport.vue";
import { patternLabel, shortId, signalLabel } from "../api.js";
import { live, selectOutput, setSource } from "../store.js";

const go = inject("go");

function sourceText(o) {
  const s = o.source || {};
  if (s.type === "pattern") return `${patternLabel(s.pattern)}${s.pluge ? " + PLUGE" : ""} · ${signalLabel(s.audio)}`;
  if (s.type === "playlist") return `Playlist ${live.playlists.find((p) => p.id === s.playlist_id)?.name || ""}${o.item ? ` · ${o.item}` : ""}`;
  return `${s.type === "still" ? "Still" : "Video"} ${o.item || live.library.find((i) => i.id === s.item_id)?.name || ""}`;
}
const disabled = (o) => ["video", "audio", "data", "key"].filter((k) => o[`master_${k}`] === false && (k !== "data" || o.anc) && (k !== "key" || o.key_mode === "fill_key"));

function open(o, tab) {
  selectOutput(o.index);
  go(tab);
}
async function preset(o, event) {
  const id = event.target.value;
  event.target.value = "";
  if (!id) return;
  await setSource(o.index, { preset: id });
}
</script>

<template>
  <div v-if="!live.outputs.length" class="empty">Waiting for the player…</div>
  <div class="grid outputs">
    <div v-for="o in live.outputs" :key="o.index" class="panel output" :class="{ selected: o.index === live.selected }">
      <h3>
        {{ o.label }} <span class="fmt">{{ o.format }}</span>
        <span class="spacer"></span>
        <span class="state-tag" :class="o.transport">{{ o.transport.toUpperCase() }}</span>
      </h3>
      <LivePicture :src="`/api/v1/outputs/${o.index}/thumbnail`" :alt="o.label" title="Select for Source and Burn-ins" @click="selectOutput(o.index)">
        <span v-if="!o.media_ready && o.source?.type !== 'pattern'" class="ov bl">waiting for the media</span>
      </LivePicture>
      <div class="srcline" :title="sourceText(o)">{{ sourceText(o) }}</div>
      <Transport :output="o" />
      <div class="statusline">
        <span>{{ o.audio_channels }} ch</span>
        <Meters :peaks="live.peaks[o.index] || []" :label="`${o.label} audio levels`" style="flex: 1; height: 26px" />
      </div>
      <div class="statusline">
        <span>grains {{ o.grains }}</span>
        <span :class="{ warn: live.underruns[o.index] }" :title="`${live.underruns[o.index] || 0} in the last minute`">underruns {{ o.underruns }}</span>
        <span>loops {{ o.loops }}</span>
        <span>key {{ o.key_mode }}</span>
        <span>flow <code :title="o.video_flow_id">{{ shortId(o.video_flow_id) }}</code></span>
      </div>
      <div v-if="disabled(o).length" class="statusline">
        <Pill :text="`sender off: ${disabled(o).join(', ')}`" kind="warn" />
      </div>
      <div class="quick">
        <select aria-label="Apply a preset" @change="preset(o, $event)">
          <option value="">Preset…</option>
          <option v-for="p in live.presets" :key="p.id" :value="p.id">{{ p.name }}</option>
        </select>
        <button class="btn small secondary" @click="open(o, 'source')">Source…</button>
        <button class="btn small secondary" @click="open(o, 'burnins')">Burn-ins…</button>
      </div>
    </div>
  </div>
</template>
