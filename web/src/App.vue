<script setup>
import { computed, onMounted, onUnmounted, provide, ref } from "vue";
import Pill from "./components/Pill.vue";
import OutputsTab from "./components/OutputsTab.vue";
import SourceTab from "./components/SourceTab.vue";
import BurninsTab from "./components/BurninsTab.vue";
import LibraryTab from "./components/LibraryTab.vue";
import PatternsTab from "./components/PatternsTab.vue";
import PlaylistsTab from "./components/PlaylistsTab.vue";
import NmosTab from "./components/NmosTab.vue";
import StatusTab from "./components/StatusTab.vue";
import SettingsTab from "./components/SettingsTab.vue";
import { activeJobs, live, startLive, stopLive } from "./store.js";

const tabs = [
  { id: "outputs", label: "Outputs", component: OutputsTab, full: true },
  { id: "source", label: "Source", component: SourceTab },
  { id: "burnins", label: "Burn-ins", component: BurninsTab },
  { id: "library", label: "Library", component: LibraryTab },
  { id: "patterns", label: "Patterns", component: PatternsTab },
  { id: "playlists", label: "Playlists", component: PlaylistsTab },
  { id: "nmos", label: "NMOS", component: NmosTab },
  { id: "status", label: "Status", component: StatusTab },
  { id: "settings", label: "Settings", component: SettingsTab },
];

const current = ref("outputs");
const tab = computed(() => tabs.find((t) => t.id === current.value));

function onHash() {
  const id = location.hash.slice(1);
  if (tabs.some((t) => t.id === id)) current.value = id;
}
function go(id) {
  current.value = id;
  location.hash = id;
}
provide("go", go);

const playing = computed(() => live.outputs.filter((o) => o.transport === "play").length);
const underruns = computed(() => Object.values(live.underruns).reduce((a, b) => a + b, 0));
const sendersOff = computed(() =>
  live.outputs.reduce(
    (n, o) =>
      n + ["video", "audio", "data", "key"].filter((k) => o[`master_${k}`] === false && (k !== "data" || o.anc) && (k !== "key" || o.key_mode === "fill_key")).length,
    0,
  ),
);
const registration = computed(() => {
  const n = live.nmos;
  if (!n) return null;
  if (!n.registry) return { text: "no registry", kind: "neutral" };
  return n.registered ? { text: "registered", kind: "ok" } : { text: "not registered", kind: "warn" };
});

onMounted(() => {
  onHash();
  window.addEventListener("hashchange", onHash);
  startLive();
});
onUnmounted(() => {
  window.removeEventListener("hashchange", onHash);
  stopLive();
});
</script>

<template>
  <header>
    <h1>mxl-test-player</h1>
    <span v-if="live.info" class="node">{{ live.info.label }} · {{ live.info.format }} · {{ live.info.outputs }} output{{ live.info.outputs > 1 ? "s" : "" }}</span>
    <span class="spacer"></span>
    <Pill v-if="live.outputs.length" :text="`${playing} of ${live.outputs.length} playing`" :kind="playing ? 'ok' : 'neutral'" title="outputs in play" />
    <Pill v-if="underruns" :text="`${underruns} underruns/min`" kind="warn" title="grains written late or repeated in the last minute" />
    <Pill v-if="sendersOff" :text="`${sendersOff} sender${sendersOff > 1 ? 's' : ''} off`" kind="warn" title="IS-05 master_enable off: the flow is not written" />
    <Pill v-if="activeJobs.length" :text="`converting ${activeJobs.length}`" kind="neutral" title="conversion jobs queued or running" />
    <Pill v-if="registration" :text="registration.text" :kind="registration.kind" title="NMOS registration" />
    <Pill :text="live.connected ? 'live' : 'offline'" :kind="live.connected ? 'ok' : 'bad'" title="/api/v1/events" />
    <span v-if="live.info" class="muted small">v{{ live.info.version }} · MXL {{ (live.info.mxl_revision || "").slice(0, 7) }}</span>
  </header>
  <div v-if="live.error" class="banner bad">{{ live.error }}</div>
  <div v-else-if="live.everConnected && !live.connected" class="banner warn">Live updates lost. Reconnecting; values refresh every 2 s meanwhile.</div>
  <div v-if="live.actionError" class="banner bad">
    {{ live.actionError }}
    <span class="spacer"></span>
    <button class="btn small secondary" @click="live.actionError = ''">Dismiss</button>
  </div>
  <div v-if="live.notice" class="banner info">
    {{ live.notice }}
    <span class="spacer"></span>
    <button class="btn small secondary" @click="live.notice = ''">Dismiss</button>
  </div>
  <nav>
    <button v-for="t in tabs" :key="t.id" :class="{ active: current === t.id }" :aria-current="current === t.id ? 'page' : undefined" @click="go(t.id)">
      {{ t.label }}<span v-if="t.id === 'library' && activeJobs.length" class="count info">{{ activeJobs.length }}</span>
    </button>
  </nav>
  <main :class="{ full: tab.full }">
    <component :is="tab.component" />
  </main>
</template>
