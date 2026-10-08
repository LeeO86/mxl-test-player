<script setup>
// NMOS (§8): the node and its registration, and per output the senders (video, audio, ANC data,
// key) with their flows and IS-05 master_enable, switchable here (PATCH /api/v1/outputs/{n}).
import { computed } from "vue";
import Pill from "./Pill.vue";
import { live, patchOutput } from "../store.js";

const n = computed(() => live.nmos);
const base = computed(() => (n.value ? `http://${n.value.host_address}:${n.value.port}` : ""));
const ROLES = [
  { key: "video", label: "Video", format: "video/v210" },
  { key: "audio", label: "Audio", format: "audio/float32" },
  { key: "data", label: "Data", format: "video/smpte291 (timecode)" },
  { key: "key", label: "Key", format: "video/v210 (key)" },
];
// The data sender exists with the ANC flow, the key sender in fill_key mode.
const senders = (o) => ROLES.filter((r) => (r.key !== "data" || o.anc) && (r.key !== "key" || o.key_mode === "fill_key"));
const format = (o, r) => (r.key === "video" && o.key_mode === "v210a" ? "video/v210a (fill and key)" : r.format);
function registration() {
  if (!n.value?.registry) return { text: "no registry", kind: "neutral" };
  return n.value.registered ? { text: "registered", kind: "ok" } : { text: "not registered", kind: "warn" };
}
</script>

<template>
  <div v-if="!n" class="empty">Loading…</div>
  <template v-else>
    <div class="grid two">
      <div class="panel">
        <h3>Node <span class="spacer"></span><Pill :text="registration().text" :kind="registration().kind" /></h3>
        <dl class="kv">
          <dt>Label</dt>
          <dd>{{ n.label }}</dd>
          <dt>Node id</dt>
          <dd><code>{{ n.node_id }}</code></dd>
          <dt>Device id</dt>
          <dd><code>{{ n.device_id }}</code></dd>
          <dt>Address</dt>
          <dd>{{ n.host_address }}:{{ n.port }} (node and connection API)</dd>
          <dt>Registry</dt>
          <dd>{{ n.registry ? `${n.registry}:${n.registry_port} (query ${n.query}:${n.query_port})` : "none: not registered" }}</dd>
          <dt>DNS-SD</dt>
          <dd>{{ n.dns_sd ? "on" : "off" }}</dd>
          <dt>Output domain</dt>
          <dd><code>{{ n.domain_id }}</code></dd>
        </dl>
        <div class="note">
          Raw resources: <a :href="`${base}/x-nmos/node/v1.3/self`" target="_blank">self</a> ·
          <a :href="`${base}/x-nmos/node/v1.3/senders`" target="_blank">senders</a> ·
          <a :href="`${base}/x-nmos/node/v1.3/flows`" target="_blank">flows</a> ·
          <a :href="`${base}/x-nmos/connection/v1.1/single/senders`" target="_blank">connection</a>
        </div>
      </div>
      <div class="panel">
        <h3>How routing works</h3>
        <p class="note" style="margin-top: 0">
          The player has senders only (transport <code>urn:x-nmos:transport:mxl</code>). Each output is a group <code>&lt;label&gt;:Video</code>,
          <code>:Audio</code>, <code>:Data</code> and <code>:Key</code>. A controller routes a receiver to a sender's <code>mxl_domain_id</code> and
          <code>mxl_flow_id</code>. Switching a sender off (IS-05 <code>master_enable</code>) stops writing its flow: a quick way to test a receiver's
          missing-signal handling. The setting is kept across a restart.
        </p>
      </div>
    </div>

    <div class="panel">
      <h3>Senders</h3>
      <table>
        <thead>
          <tr><th>Output</th><th>Essence</th><th>Format</th><th>Sender id</th><th>Flow id</th><th>master_enable</th></tr>
        </thead>
        <tbody>
          <template v-for="o in live.outputs" :key="o.index">
            <tr v-for="(r, i) in senders(o)" :key="`${o.index}-${r.key}`" :class="{ dim: o[`master_${r.key}`] === false }">
              <td>{{ i === 0 ? o.label : "" }}</td>
              <td>{{ r.label }}</td>
              <td class="small">{{ format(o, r) }}</td>
              <td><code>{{ o[`${r.key}_sender_id`] }}</code></td>
              <td><code>{{ o[`${r.key}_flow_id`] }}</code></td>
              <td class="nowrap">
                <Pill :text="o[`master_${r.key}`] === false ? 'off' : 'on'" :kind="o[`master_${r.key}`] === false ? 'warn' : 'ok'" />
                <button class="btn small secondary" style="margin-left: 0.4rem" @click="patchOutput(o.index, { [`master_${r.key}`]: o[`master_${r.key}`] === false })">
                  {{ o[`master_${r.key}`] === false ? "Enable" : "Disable" }}
                </button>
              </td>
            </tr>
          </template>
        </tbody>
      </table>
    </div>
  </template>
</template>
