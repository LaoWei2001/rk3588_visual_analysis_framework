"""A live preview must regain a short latency after recovering from starvation."""
from pathlib import Path
import subprocess


def test_recovery_reserve_is_bounded_and_returns_to_live_latency(tmp_path):
    frontend = Path(__file__).resolve().parents[1]
    compiled = tmp_path/'playback.mjs'
    result = subprocess.run(['node', '-e', r'''
        const ts = require('typescript'), fs = require('fs');
        process.stdout.write(ts.transpileModule(fs.readFileSync(process.argv[1], 'utf8'), {
            compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.ES2020}
        }).outputText);
    ''', str(frontend/'src/pages/livePlayback.ts')], cwd=frontend, capture_output=True, text=True, check=True)
    compiled.write_text(result.stdout)
    result = subprocess.run(['node', '--input-type=module', '-e', f'''
        import assert from 'node:assert/strict';
        import {{LivePlaybackBuffer, livePlaybackRate, livePlaybackTarget,
          LIVE_TARGET_LATENCY_SECONDS as target, LIVE_MAX_RECOVERY_BUFFER_SECONDS as cap}} from '{compiled.as_uri()}';
        const buffer = new LivePlaybackBuffer();
        assert(!buffer.starved(1));
        assert(!buffer.starved(NaN));
        assert.equal(buffer.targetSeconds, target);
        assert(buffer.starved(0));
        assert(buffer.targetSeconds > target && buffer.targetSeconds <= cap);
        assert(!buffer.starved(0));
        assert(!buffer.resume(.1));
        assert(!buffer.resume(NaN));
        assert(buffer.resume(cap));
        for(let i=0;i<20;i++) {{ assert(buffer.starved(0)); assert(buffer.resume(cap)); }}
        assert.equal(buffer.targetSeconds, cap);
        assert(cap <= .6); // Repeated stalls must not create a 1.8-second reserve.
        buffer.observePlayback(1000, true);
        buffer.observePlayback(5000, true);
        assert.equal(buffer.targetSeconds, cap);
        buffer.observePlayback(5100, false); // paused/seeking/hidden resets stability
        buffer.observePlayback(10000, true);
        buffer.observePlayback(14000, true);
        assert.equal(buffer.targetSeconds, cap);
        for(let now=14250;now<=22000;now+=250) buffer.observePlayback(now,true);
        assert.equal(buffer.targetSeconds,target); // Real frame presentation decays the reserve.
        for(let now=22250;now<=40000;now+=250) buffer.observePlayback(now,true);
        assert.equal(buffer.targetSeconds,target);
        assert.equal(livePlaybackRate(10,10+target+.02),1);
        assert(livePlaybackRate(10,10+target+.3)>1);
        assert.equal(livePlaybackRate(NaN,11),1);
        assert.equal(livePlaybackTarget(10,9,10.5,true),null);
        assert(Math.abs(livePlaybackTarget(10,9,12,true)- (12-target))<1e-9);
        assert.equal(livePlaybackTarget(0,10,10.1,false),null);
        assert(Math.abs(livePlaybackTarget(0,10,11,false)-(11-target))<1e-9);
        assert(Math.abs(livePlaybackTarget(1,10,11,true)-(11-target))<1e-9);
    '''], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
