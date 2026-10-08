<script setup>
// Transport of one output (§7): play, pause, stop, restart, step, loop, and the position in the
// current item with seek (a click on the bar) for videos.
import { computed } from "vue";
import Segmented from "./Segmented.vue";
import { fmtSeconds, formatFps } from "../api.js";
import { patchOutput, seek, transport } from "../store.js";

const props = defineProps({ output: { type: Object, required: true } });

const STATES = [
  { value: "play", label: "Play" },
  { value: "pause", label: "Pause" },
  { value: "stop", label: "Stop", title: "black and silence" },
];
const fps = computed(() => formatFps(props.output.format));
const hasLength = computed(() => props.output.frames > 1);
const seekable = computed(() => hasLength.value && props.output.source?.type === "video");

function onBar(event) {
  if (!seekable.value) return;
  const rect = event.currentTarget.getBoundingClientRect();
  const frac = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
  seek(props.output.index, Math.min(props.output.frames - 1, frac * props.output.frames));
}
</script>

<template>
  <div class="transport">
    <Segmented :options="STATES" :model-value="output.transport" label="Transport" @update:model-value="(a) => transport(output.index, a)" />
    <button class="btn small secondary" title="Play from the start" @click="transport(output.index, 'restart')">Restart</button>
    <button class="btn small secondary" title="Pause and show the next frame" @click="transport(output.index, 'step')">Step</button>
    <button class="btn small secondary" :aria-pressed="output.loop" title="Loop videos (else hold the last frame)" @click="patchOutput(output.index, { loop: !output.loop })">
      Loop
    </button>
  </div>
  <div v-if="hasLength" class="position">
    <span>{{ fmtSeconds(output.position / fps) }}</span>
    <div
      class="bar"
      :class="{ seekable }"
      :title="seekable ? 'Click to seek' : ''"
      role="progressbar"
      :aria-valuenow="output.position"
      aria-valuemin="0"
      :aria-valuemax="output.frames"
      @click="onBar"
    >
      <div :style="{ width: `${output.progress * 100}%` }"></div>
    </div>
    <span>−{{ fmtSeconds(output.remaining_s) }}</span>
  </div>
</template>
