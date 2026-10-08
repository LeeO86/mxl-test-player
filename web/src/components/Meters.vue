<script setup>
// Peak meters per audio channel (dBFS, −60 to 0) from the WebSocket.
import { computed } from "vue";
import { peakDb } from "../api.js";

const props = defineProps({
  peaks: { type: Array, default: () => [] },
  label: { type: String, default: "Audio levels" },
});

const bars = computed(() =>
  props.peaks.map((p, c) => {
    const db = peakDb(p);
    return {
      pct: Math.max(0, Math.min(100, ((db + 60) / 60) * 100)),
      kind: db >= -9 ? "hot" : db > -17.5 ? "warm" : "",
      title: `channel ${c + 1}: ${db > -100 ? db.toFixed(1) : "-inf"} dBFS`,
    };
  }),
);
</script>

<template>
  <div class="meters" role="img" :aria-label="label">
    <div v-for="(b, c) in bars" :key="c" class="ch" :title="b.title">
      <div class="fill" :class="b.kind" :style="{ height: b.pct + '%' }"></div>
    </div>
  </div>
</template>
