"""Exercise actual graph converters for multi-camera channel settings."""
from pathlib import Path
import subprocess


def test_channel_settings_roundtrip_and_thread_overrides():
    frontend = Path(__file__).resolve().parents[1]
    script = r"""
const assert = require('node:assert/strict');
const fs = require('node:fs');
const ts = require('typescript');
for (const ext of ['.ts', '.tsx']) require.extensions[ext] = (mod, name) => {
  mod._compile(ts.transpileModule(fs.readFileSync(name, 'utf8'), {
    compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020,
      jsx: ts.JsxEmit.ReactJSX}
  }).outputText, name);
};
require.extensions['.css'] = () => {};
const {configToGraph} = require('./src/utils/configToGraph.ts');
const {graphToConfig} = require('./src/utils/graphToConfig.ts');
const flags = [
  {enable: false, infer_enable: false, swap_rb: true},
  {enable: true, infer_enable: true, swap_rb: false},
  {enable: 0, infer_enable: 0, swap_rb: 1}, {},
];
const original = {global: {channel_threads: 3}, channels: flags.map((switches, id) => ({
  id, ...switches, ...(id === 0 ? {threads: 2} : id === 1 ? {threads: -1} : {}),
  stream: {src_type: 'rtsp', url: 'rtsp://example.invalid/cam' + id, video_enc: 'h264'},
  models: [{id: 'person', enable: true, model_type: 'yolov8_det',
    model_path: 'assets/person.rknn', label_path: 'assets/person.txt',
    obj_thresh: 0.9, nms_thresh: 0.45, detect_classes: ['person'], npu_core: -1}]
}))};
const save = graph => graphToConfig(graph.nodes, graph.edges, graph.roiMapping, graph.globalSettings).config;
const checkFlags = config => config.channels.forEach((ch, i) => {
  for (const key of ['enable'])
    assert.deepEqual(ch[key], flags[i][key]);
});
const graph = configToGraph(original);
const first = save(graph);
checkFlags(first);
assert.ok(first.channels.every(channel => !('infer_enable' in channel) && !('swap_rb' in channel)));
assert.ok(graph.nodes.filter(node => node.type === 'stream').every(node => !('infer_enable' in node.data)));
const model = graph.nodes.find(node => node.type === 'model');
model.data.enable = false;
assert.equal(save(graph).channels[0].models[0].enable, false);
model.data.enable = true;
assert.equal(save(graph).channels[0].models[0].enable, true);
assert.equal(first.global.channel_threads, 3);
assert.deepEqual(first.channels.map(ch => ch.threads), [2, -1, undefined, undefined]);
const streams = graph.nodes.filter(n => n.type === 'stream');
streams[0].data.threads = 7;
streams[1].data.threads = undefined;
streams[3].data.threads = 4;
const second = save(graph);
checkFlags(second);
assert.deepEqual(second.channels.map(ch => ch.threads), [7, undefined, undefined, 4]);
assert.equal(second.global.channel_threads, 3);
for (const ch of second.channels) {
  assert.ok(!Object.hasOwn(ch.stream, 'threads'));
  assert.equal(ch.models[0].obj_thresh, 0.9);
  assert.ok(!Object.hasOwn(ch.models[0], 'threads'));
}
const reloaded = configToGraph(second);
assert.deepEqual(reloaded.nodes.filter(n => n.type === 'stream').map(n => n.data.threads),
  [7, undefined, undefined, 4]);
checkFlags(save(reloaded));
// Retired form metadata and inline contracts must never return on save, even for disabled reports.
const oldPolicy = {enabled:false, parameters:[{id:'retired'}], old_option:true,
  image_selection:{mode:'selected', channel_ids:[0], retired:true},
  merge_window_sec:7, video_pre_sec:4,
  deliveries:[{id:'d',enabled:false,connection_id:'server',contract_id:'alarm',contract_label:'Alarm',
    contract_revision:'revision',media:['annotated_image'],when:{event_types:['alarm'],retired:true},
    mapping:[],request:{},success:{},adapter:'http',retired:true}]};
const reportConfig = {global:{global_logics:[{instance_id:'summary',logic:'global_default',channels:[0],
  report_policy:oldPolicy,report_parameters:{device_id:'global'},logic_parameters:{business:1}}]},
  channels:[{...original.channels[0],logic:'logic_default',report_policy:oldPolicy,
    report_parameters:{device_id:'channel'},logic_parameters:{business:2}}]};
const snapshot = JSON.stringify(reportConfig);
const cleaned = save(configToGraph(reportConfig));
assert.equal(JSON.stringify(reportConfig),snapshot);
for (const owner of [cleaned.channels[0],cleaned.global.global_logics[0]]) {
  const policy = owner.report_policy;
  assert.equal(policy.enabled,false);
  assert.equal(policy.merge_window_sec,7);
  assert.equal(policy.video_pre_sec,4);
  assert.ok(!('parameters' in policy));
  assert.ok(!('old_option' in policy));
  assert.deepEqual(policy.image_selection,{mode:'selected',channel_ids:[0]});
  assert.deepEqual(policy.deliveries[0],{id:'d',enabled:false,connection_id:'server',contract_id:'alarm',
    contract_label:'Alarm',contract_revision:'revision',media:['annotated_image'],when:{event_types:['alarm']}});
}
assert.ok(!('report_parameters' in cleaned.channels[0]));
assert.ok(!('report_parameters' in cleaned.global.global_logics[0]));
assert.deepEqual(cleaned.channels[0].logic_parameters,{business:2});
assert.deepEqual(cleaned.global.global_logics[0].logic_parameters,{business:1});

// Loading a sparse file must preserve native behavior, including disabled globals and non-looping files.
const sparse = save(configToGraph({global:{global_logics:[{instance_id:'g',logic:'global_default',channels:[4]}]},
  channels:[{id:4,stream:{src_type:'file',url:'a.mp4'},models:[]}]}));
assert.equal(sparse.global.global_logics[0].enable,false);
assert.equal(sparse.global.global_logics[0].poll_interval_ms,100);
assert.deepEqual(sparse.global.global_logics[0].channels,[4]); // Direct stream -> global logic survives.
assert.equal(sparse.channels[0].stream.loop,false);
const {NATIVE_GLOBAL_DEFAULTS} = require('./src/components/GlobalSettingsPanel.tsx');
const native = fs.readFileSync('../../engine/src/application/config/config.h','utf8');
const defaults = {};
for (const [,type,key,raw] of native.matchAll(/(bool|int|float|std::string) (\w+) = ([^;]+);/g)) {
  if (!(key in NATIVE_GLOBAL_DEFAULTS)) continue;
  defaults[key] = type === 'std::string' ? JSON.parse(raw) : type === 'bool' ? Number(raw === 'true') : Number(raw.replace(/f$/, ''));
}
assert.deepEqual(NATIVE_GLOBAL_DEFAULTS,defaults);
for (const [key,value] of Object.entries(defaults)) assert.deepEqual(sparse.global[key],value,key);
const rtsp = configToGraph({global:{rtsp_codec:'h265',rtsp_encoder:'hw',rtsp_path:'/custom',rtsp_port:9554,rtsp_bitrate:2048,enable_pause_key:1},channels:original.channels});
assert.equal(save(rtsp).global.rtsp_codec,'h265');
assert.equal(save(rtsp).global.rtsp_path,'/custom');
assert.equal(save(rtsp).global.enable_pause_key,1);
const {updateNodeConfiguration} = require('./src/utils/reportSettings.ts');
const reports = configToGraph({global:{},channels:[{...original.channels[1],logic:'logic_default',
 report_policy:{enabled:true,video_pre_sec:3,deliveries:[
  {id:'a',enabled:true,media:['video'],contract_id:'one'},
  {id:'b',enabled:false,media:['video'],contract_id:'two'}]},report_parameters:{old:1}}]});
const reportNodes = reports.nodes.filter(n => n.type === 'report');
const changed = reportNodes[1]; // Editing the second (disabled) delivery updates the shared native policy.
reports.nodes = updateNodeConfiguration(reports.nodes,reports.edges,changed.id,{
 report_policy:{...changed.data.report_policy,video_pre_sec:9,video_overlay:'none'}});
const policy = save(reports).channels[0].report_policy;
assert.equal(policy.video_pre_sec,9);
assert.equal(policy.video_overlay,'none');
assert.deepEqual(policy.deliveries.map(d=>[d.id,d.enabled,d.contract_id]),[['a',true,'one'],['b',false,'two']]);
assert.ok(!('report_parameters' in save(reports).channels[0]));

"""
    subprocess.run(["node", "-e", script], cwd=frontend, check=True, timeout=30)
