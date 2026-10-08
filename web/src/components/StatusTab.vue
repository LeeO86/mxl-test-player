<script setup>
// Status (§9, §10): health probes, per-output counters with recent underruns, a read-back of
// the newest grain from MXL (picture value, hash, ANC timecode), and the library metrics.
import { onMounted, onUnmounted, ref } from "vue";
import Pill from "./Pill.vue";
import { api, fmtBytes, probe, shortId } from "../api.js";
import { act, activeJobs, live } from "../store.js";

const health = ref({ livez: null, readyz: null, statusz: null });
const metrics = ref({});
const readback = ref({}); // output index -> probe answer
let timer = null;

async function load() {
  const [livez, readyz] = await Promise.all([probe("/livez"), probe("/readyz")]);
  let statusz = null;
  try {
    statusz = await api.get("/statusz");
  } catch {
    /* shown as not ok */
  }
  health.value = { livez, readyz, statusz };
  try {
    const text = await api.text("/metrics");
    const pick = (name) => Number(new RegExp(`^${name} (\\S+)$`, "m").exec(text)?.[1] ?? NaN);
    metrics.value = { items: pick("mxl_test_player_library_items"), bytes: pick("mxl_test_player_library_bytes"), uploaded: pick("mxl_test_player_upload_bytes_total") };
  } catch {
    /* keep the last values */
  }
}
onMounted(() => {
  load();
  timer = setInterval(load, 5000);
});
onUnmounted(() => clearInterval(timer));

async function readBack(o) {
  const result = await act(() => api.get(`/api/v1/outputs/${o.index}/probe`));
  if (result) readback.value = { ...readback.value, [o.index]: { ...result, at: new Date().toLocaleTimeString() } };
}
const code = (c) => ({ text: c ? String(c) : "no answer", kind: c === 200 ? "ok" : c ? "warn" : "bad" });
const failedJobs = () => live.jobs.filter((j) => j.state === "failed").length;
</script>

<template>
  <div class="grid cards">
    <div class="panel">
      <h3>Health</h3>
      <dl class="kv">
        <dt>/livez</dt>
        <dd><Pill :text="code(health.livez).text" :kind="code(health.livez).kind" /></dd>
        <dt>/readyz</dt>
        <dd>
          <Pill :text="code(health.readyz).text" :kind="code(health.readyz).kind" />
          <span v-if="health.readyz === 503" class="muted small"> the registry does not list the node yet</span>
        </dd>
        <dt>/statusz</dt>
        <dd><Pill :text="health.statusz?.ok ? 'ok' : 'not ok'" :kind="health.statusz?.ok ? 'ok' : 'bad'" /></dd>
        <dt>Live updates</dt>
        <dd><Pill :text="live.connected ? 'connected' : 'reconnecting'" :kind="live.connected ? 'ok' : 'warn'" /></dd>
        <dt>Output domain</dt>
        <dd><code>{{ health.statusz?.domain || "–" }}</code></dd>
      </dl>
    </div>
    <div class="panel">
      <h3>Library</h3>
      <dl class="kv">
        <dt>Items</dt>
        <dd>{{ Number.isFinite(metrics.items) ? metrics.items : live.library.length }}</dd>
        <dt>Size on disk</dt>
        <dd>{{ Number.isFinite(metrics.bytes) ? fmtBytes(metrics.bytes) : "–" }}</dd>
        <dt>Uploads in progress</dt>
        <dd>{{ Number.isFinite(metrics.uploaded) ? fmtBytes(metrics.uploaded) : "–" }}</dd>
        <dt>Conversions</dt>
        <dd>{{ activeJobs.length }} running or queued<span v-if="failedJobs()">, <span class="msg err">{{ failedJobs() }} failed</span></span></dd>
      </dl>
      <div class="note">All counters: <a href="/metrics" target="_blank">/metrics</a> (Prometheus).</div>
    </div>
  </div>

  <div class="panel">
    <h3>Outputs</h3>
    <table>
      <thead>
        <tr>
          <th>Output</th>
          <th>Transport</th>
          <th class="num">Grains</th>
          <th class="num" title="grains written late or repeated">Underruns</th>
          <th class="num">Last minute</th>
          <th class="num">Loops</th>
          <th class="num">RAM clip</th>
          <th>Media</th>
          <th>Read back from MXL</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="o in live.outputs" :key="o.index">
          <td><strong>{{ o.label }}</strong> <span class="muted small">{{ o.format }}</span></td>
          <td><span class="state-tag" :class="o.transport">{{ o.transport.toUpperCase() }}</span></td>
          <td class="num">{{ o.grains }}</td>
          <td class="num">{{ o.underruns }}</td>
          <td class="num"><Pill :text="String(live.underruns[o.index] || 0)" :kind="live.underruns[o.index] ? 'warn' : 'ok'" /></td>
          <td class="num">{{ o.loops }}</td>
          <td class="num">{{ o.ram_bytes ? fmtBytes(o.ram_bytes) : "–" }}</td>
          <td>
            <Pill v-if="o.source?.type === 'pattern'" text="pattern" kind="neutral" />
            <Pill v-else :text="o.media_ready ? 'ready' : 'waiting'" :kind="o.media_ready ? 'ok' : 'warn'" />
          </td>
          <td class="small">
            <button class="btn small secondary" @click="readBack(o)">Read</button>
            <template v-if="readback[o.index]">
              <span v-if="!readback[o.index].ok" class="msg err"> no grain (sender off?)</span>
              <span v-else>
                grain {{ readback[o.index].index }} · Y {{ readback[o.index].y }} Cb {{ readback[o.index].cb }} Cr {{ readback[o.index].cr }} ·
                <span :title="readback[o.index].fnv">hash {{ shortId(readback[o.index].fnv) }}</span> · TC
                <span class="mono">{{ readback[o.index].anc_ok ? readback[o.index].anc_timecode : "–" }}</span>
                <span class="muted"> ({{ readback[o.index].at }})</span>
              </span>
            </template>
          </td>
        </tr>
      </tbody>
    </table>
    <p class="note">
      Read reads the grain written two grains ago back from the output's video and ANC flows: the centre pixel (10-bit Y, Cb, Cr), a hash of the
      grain, and the timecode in the ANC flow.
    </p>
  </div>
</template>
