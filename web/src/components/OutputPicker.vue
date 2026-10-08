<script setup>
// The output a page edits, with each output's transport state. Kept in this browser.
import { computed } from "vue";
import Segmented from "./Segmented.vue";
import { live, selectOutput, selectedOutput } from "../store.js";

const STATE_TEXT = { play: "PLAY", pause: "PAUSE", stop: "STOP" };
const options = computed(() => live.outputs.map((o) => ({ value: o.index, label: o.label, state: o.transport })));
</script>

<template>
  <div class="group">
    <span class="caption">Output</span>
    <Segmented :options="options" :model-value="selectedOutput?.index" label="Output" @update:model-value="selectOutput">
      <template #default="{ option }">
        {{ option.label }}<span class="out-state" :class="option.state">{{ STATE_TEXT[option.state] }}</span>
      </template>
    </Segmented>
  </div>
</template>
