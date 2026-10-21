"""Buffer recovery preserves a bounded reserve instead of playing in bursts."""
from pathlib import Path
import subprocess


def test_adaptive_reserve_is_bounded_and_not_consumed_by_catchup(tmp_path):
    frontend = Path(__file__).resolve().parents[1]
    compiled = tmp_path/'playback.mjs'
    result = subprocess.run(['node', '-e', '''
        const ts = require('typescript'), fs = require('fs');
        process.stdout.write(ts.transpileModule(fs.readFileSync(process.argv[1], 'utf8'), {
            compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.ES2020}
        }).outputText);
    ''', str(frontend/'src/pages/livePlayback.ts')], cwd=frontend, capture_output=True, text=True, check=True)
    compiled.write_text(result.stdout)
    result = subprocess.run(['node', '--input-type=module', '-e', f'''
        import assert from 'node:assert/strict';
        import {{LivePlaybackBuffer, livePlaybackRate, livePlaybackTarget}} from '{compiled.as_uri()}';
        const buffer = new LivePlaybackBuffer();
        assert(!buffer.starved(1));
        assert(!buffer.starved(NaN));
        assert.equal(buffer.targetSeconds, .6);
        assert(buffer.starved(0));
        assert.equal(buffer.targetSeconds, 1);
        assert(!buffer.starved(0));
        assert(!buffer.resume(.8));
        assert(!buffer.resume(NaN));
        assert(buffer.resume(1));
        for(let i=0;i<20;i++) {{ assert(buffer.starved(0)); assert(buffer.resume(2)); }}
        assert.equal(buffer.targetSeconds, 1.8);
        assert.equal(livePlaybackRate(10,11.8,buffer.targetSeconds),1);
        assert.equal(livePlaybackRate(10,13,buffer.targetSeconds),1.05);
        assert.equal(livePlaybackTarget(10,9,15,true,buffer.targetSeconds),13.2);
        assert.equal(livePlaybackTarget(0,10,11,false,buffer.targetSeconds),null);
        assert.equal(livePlaybackTarget(0,10,12,false,buffer.targetSeconds),10.2);
    '''], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
