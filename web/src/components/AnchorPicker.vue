<script setup>
// Nine anchor positions (tl … br) as a 3 × 3 grid.
import { ANCHORS } from "../api.js";

defineProps({ modelValue: { type: String, default: "tl" }, label: { type: String, default: "Position" } });
defineEmits(["update:modelValue"]);
const NAMES = { t: "top", m: "middle", b: "bottom", l: "left", c: "centre", r: "right" };
const title = (a) => `${NAMES[a[0]]} ${NAMES[a[1]]}`;
</script>

<template>
  <div class="anchors" role="group" :aria-label="label">
    <button
      v-for="a in ANCHORS"
      :key="a"
      type="button"
      :class="{ active: a === modelValue }"
      :aria-pressed="a === modelValue"
      :title="title(a)"
      :aria-label="title(a)"
      @click="$emit('update:modelValue', a)"
    ></button>
  </div>
</template>
