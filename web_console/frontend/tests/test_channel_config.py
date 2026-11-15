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
  for (const key of ['enable', 'infer_enable', 'swap_rb'])
    assert.deepEqual(ch[key], flags[i][key]);
});
const graph = configToGraph(original);
const first = save(graph);
checkFlags(first);
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
"""
    subprocess.run(["node", "-e", script], cwd=frontend, check=True, timeout=30)
