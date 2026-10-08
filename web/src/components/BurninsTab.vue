<script setup>
// Burn-ins of the selected output (§5.4–5.6): the standard burn-ins, up to 8 text layers and up
// to 2 moving boxes. Every change goes to the picture at once (80 ms after the last keystroke);
// the flows and the grain timing stay the same.
import { computed, watch } from "vue";
import AnchorPicker from "./AnchorPicker.vue";
import LivePicture from "./LivePicture.vue";
import OutputPicker from "./OutputPicker.vue";
import Segmented from "./Segmented.vue";
import { FONTS, newBox, newTextLayer, PATHS, PLACEHOLDERS } from "../api.js";
import { burninPending, drafts, editBurnin, live, selectedOutput, syncBurnin } from "../store.js";

const o = selectedOutput;
watch(
  () => o.value && [o.value.index, o.value.burnin],
  () => o.value && syncBurnin(o.value.index, o.value.burnin),
  { immediate: true, deep: true },
);
const d = computed(() => drafts.burnin[o.value?.index]);
const b = computed(() => d.value?.value);
const changed = () => editBurnin(o.value.index);

const STANDARD = [
  { key: "timecode", label: "Timecode", title: "top centre, the output's timecode" },
  { key: "utc_clock", label: "UTC clock", title: "top right" },
  { key: "local_clock", label: "Local clock", title: "top right" },
  { key: "frame_counter", label: "Frame counter", title: "bottom right" },
  { key: "item", label: "Item and loop", title: "bottom left" },
  { key: "flow_id", label: "Flow id", title: "middle left" },
];
const sprites = computed(() => live.library.filter((i) => i.type === "sprite" || i.type === "still"));

function addText() {
  b.value.texts.push(newTextLayer());
  changed();
}
function addBox() {
  b.value.boxes.push(newBox());
  changed();
}
function remove(list, i) {
  list.splice(i, 1);
  changed();
}
function insert(layer, placeholder) {
  layer.text = `${layer.text}${layer.text && !layer.text.endsWith(" ") ? " " : ""}${placeholder}`;
  changed();
}
// Speed and size are fractions in the API, percent here.
const pct = (v) => Math.round(v * 1000) / 10;
function setPct(box, key, value) {
  const n = Number(value);
  if (!Number.isFinite(n)) return;
  box[key] = n / 100;
  changed();
}
</script>

<template>
  <div class="toolbar">
    <OutputPicker />
    <span v-if="d?.error" class="msg err" style="margin: 0">Not applied: {{ d.error }}</span>
    <span v-else-if="o && burninPending(o.index)" class="muted small">applying…</span>
  </div>
  <div v-if="!o || !b" class="empty">Waiting for the player…</div>
  <div v-else class="editor">
    <div class="main">
      <div class="panel">
        <h3>Standard burn-ins</h3>
        <div class="row tight">
          <label class="check"><input v-model="b.label" type="checkbox" @change="changed" /> Label</label>
          <span class="muted small">at</span>
          <AnchorPicker v-model="b.label_anchor" label="Label position" @update:model-value="changed" />
        </div>
        <div class="row tight">
          <label v-for="s in STANDARD" :key="s.key" class="check" :title="s.title"><input v-model="b[s.key]" type="checkbox" @change="changed" /> {{ s.label }}</label>
        </div>
        <label class="check" title="§7.2: burn-ins are not drawn on keyed stills unless this is on">
          <input v-model="b.on_keyed_stills" type="checkbox" @change="changed" /> Also on keyed stills
        </label>
      </div>

      <div class="panel">
        <h3>
          Text layers <span class="muted">{{ b.texts.length }} of 8</span>
          <span class="spacer"></span>
          <button class="btn small" :disabled="b.texts.length >= 8" @click="addText">Add text</button>
        </h3>
        <div v-if="!b.texts.length" class="empty">No text layers.</div>
        <div v-for="(t, i) in b.texts" :key="i" class="layer">
          <div class="head">
            Text {{ i + 1 }}
            <span class="spacer"></span>
            <button class="btn small danger" @click="remove(b.texts, i)">Remove</button>
          </div>
          <textarea v-model="t.text" :aria-label="`Text ${i + 1}`" @input="changed"></textarea>
          <div class="chips" style="margin-top: 0.3rem">
            <button v-for="p in PLACEHOLDERS" :key="p" type="button" :title="`insert ${p}`" @click="insert(t, p)">{{ p }}</button>
          </div>
          <div class="inline" style="margin-top: 0.5rem">
            <AnchorPicker v-model="t.anchor" :label="`Text ${i + 1} position`" @update:model-value="changed" />
            <label :for="`tx${i}`">x %</label>
            <input :id="`tx${i}`" v-model.number="t.offset_x" type="number" step="0.5" @input="changed" />
            <label :for="`ty${i}`">y %</label>
            <input :id="`ty${i}`" v-model.number="t.offset_y" type="number" step="0.5" @input="changed" />
            <label :for="`ts${i}`">size %</label>
            <input :id="`ts${i}`" v-model.number="t.size" type="number" min="0.5" max="50" step="0.5" @input="changed" />
          </div>
          <div class="inline" style="margin-top: 0.4rem">
            <select v-model="t.font" :aria-label="`Text ${i + 1} font`" style="width: 11rem" @change="changed">
              <option v-for="f in FONTS" :key="f">{{ f }}</option>
            </select>
            <input v-model="t.color" type="color" :aria-label="`Text ${i + 1} colour`" @input="changed" />
            <label :for="`to${i}`">opacity</label>
            <input :id="`to${i}`" v-model.number="t.opacity" type="range" min="0" max="1" step="0.05" @input="changed" />
          </div>
          <div class="inline" style="margin-top: 0.4rem">
            <label class="check"><input v-model="t.box" type="checkbox" @change="changed" /> Box</label>
            <input v-model="t.box_color" type="color" :disabled="!t.box" :aria-label="`Text ${i + 1} box colour`" @input="changed" />
            <input v-model.number="t.box_opacity" type="range" min="0" max="1" step="0.05" :disabled="!t.box" :aria-label="`Text ${i + 1} box opacity`" @input="changed" />
            <label :for="`tp${i}`">padding %</label>
            <input :id="`tp${i}`" v-model.number="t.box_padding" type="number" min="0" max="10" step="0.1" :disabled="!t.box" @input="changed" />
          </div>
          <div class="inline" style="margin-top: 0.2rem">
            <label class="check"><input v-model="t.outline" type="checkbox" @change="changed" /> Outline</label>
            <input v-model="t.outline_color" type="color" :disabled="!t.outline" :aria-label="`Text ${i + 1} outline colour`" @input="changed" />
            <label class="check"><input v-model="t.shadow" type="checkbox" @change="changed" /> Shadow</label>
          </div>
        </div>
      </div>

      <div class="panel">
        <h3>
          Moving boxes <span class="muted">{{ b.boxes.length }} of 2</span>
          <span class="spacer"></span>
          <button class="btn small" :disabled="b.boxes.length >= 2" @click="addBox">Add box</button>
        </h3>
        <div v-if="!b.boxes.length" class="empty">No moving box. It proves the picture is live: a frozen output shows at once.</div>
        <div v-for="(box, i) in b.boxes" :key="i" class="layer">
          <div class="head">
            <label class="check" style="margin: 0"><input v-model="box.enabled" type="checkbox" @change="changed" /> Box {{ i + 1 }}</label>
            <span class="spacer"></span>
            <button class="btn small danger" @click="remove(b.boxes, i)">Remove</button>
          </div>
          <div class="inline">
            <select v-model="box.content" :aria-label="`Box ${i + 1} content`" style="width: 14rem" @change="changed">
              <option value="builtin">Built-in box with frame counter</option>
              <option v-for="s in sprites" :key="s.id" :value="s.id">{{ s.name }} ({{ s.type === "sprite" ? "animated" : "still" }})</option>
            </select>
            <input v-model="box.color" type="color" :disabled="box.content !== 'builtin'" :aria-label="`Box ${i + 1} colour`" @input="changed" />
            <label class="check"><input v-model="box.include_in_key" type="checkbox" @change="changed" /> In the key</label>
          </div>
          <div style="margin-top: 0.4rem">
            <Segmented v-model="box.path" :options="PATHS" :label="`Box ${i + 1} path`" class="small" @update:model-value="changed" />
          </div>
          <div class="inline" style="margin-top: 0.4rem">
            <label :for="`bs${i}`" title="percent of the frame width per second">speed %/s</label>
            <input :id="`bs${i}`" :value="pct(box.speed)" type="number" min="0" max="200" step="1" @input="setPct(box, 'speed', $event.target.value)" />
            <label :for="`bz${i}`" title="percent of the frame height">size %</label>
            <input :id="`bz${i}`" :value="pct(box.size)" type="number" min="1" max="100" step="1" @input="setPct(box, 'size', $event.target.value)" />
            <label :for="`bo${i}`">opacity</label>
            <input :id="`bo${i}`" v-model.number="box.opacity" type="range" min="0" max="1" step="0.05" @input="changed" />
          </div>
          <div class="inline" style="margin-top: 0.4rem">
            <label :for="`bx${i}`" title="0–1 of the travel span">start x</label>
            <input :id="`bx${i}`" v-model.number="box.start_x" type="number" min="0" max="1" step="0.05" @input="changed" />
            <label :for="`by${i}`">start y</label>
            <input :id="`by${i}`" v-model.number="box.start_y" type="number" min="0" max="1" step="0.05" @input="changed" />
          </div>
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
        <LivePicture :src="`/api/v1/outputs/${o.index}/thumbnail`" :interval="500" :alt="o.label" />
        <p class="note">Placeholders are filled in every frame. Sizes and offsets are percent of the frame.</p>
      </div>
    </div>
  </div>
</template>
