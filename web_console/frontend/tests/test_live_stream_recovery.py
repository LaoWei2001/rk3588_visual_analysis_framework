"""Exercise recovery timers and progress with deterministic media events."""

from pathlib import Path
import subprocess

import pytest

FRONTEND = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def recovery_module(tmp_path_factory):
    compiled = tmp_path_factory.mktemp('stream-recovery') / 'recovery.mjs'
    result = subprocess.run(['node', '-e', '''
        const ts = require('typescript'), fs = require('fs');
        process.stdout.write(ts.transpileModule(fs.readFileSync(process.argv[1], 'utf8'), {
            compilerOptions: {target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.ES2020}
        }).outputText);
    ''', str(FRONTEND / 'src/pages/liveStreamRecovery.ts')], cwd=FRONTEND, capture_output=True, text=True, check=True)
    compiled.write_text(result.stdout)
    return compiled.as_uri()


def run(recovery_module, script):
    result = subprocess.run(['node', '--input-type=module', '-e',
        "import assert from 'node:assert/strict';\n"
        f"import {{ PlaybackProgress, StreamOperationTimeout, streamRetryDelay, sourceBufferOperation, waitForSourceOpen, readStreamChunk }} from '{recovery_module}';\n"
        + script], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


def test_backoff_increases_with_jitter_and_is_bounded(recovery_module):
    run(recovery_module, '''
        assert.equal(streamRetryDelay(1, 0), 375);
        assert.equal(streamRetryDelay(1, 1), 500);
        for (let i = 2; i <= 5; i++) assert(streamRetryDelay(i, 0) > streamRetryDelay(i - 1, 1));
        for (let i = 1; i <= 100; i++) assert(streamRetryDelay(i, 1) <= 15000);
        assert.notEqual(streamRetryDelay(5, 0), streamRetryDelay(5, 1));
    ''')


def test_buffer_cancel_timeout_success_and_quota_error(recovery_module):
    run(recovery_module, '''
        class Target extends EventTarget {
            active = new Set();
            addEventListener(type, fn, options) { this.active.add(fn); super.addEventListener(type, fn, options); }
            removeEventListener(type, fn) { this.active.delete(fn); super.removeEventListener(type, fn); }
        }
        const target = new Target(), abort = new AbortController();
        const pending = sourceBufferOperation(target, () => {}, abort.signal, 1000);
        abort.abort();
        await assert.rejects(pending, e => e.name === 'AbortError');
        assert.equal(target.active.size, 0);
        const signal = new AbortController().signal;
        await assert.rejects(sourceBufferOperation(target, () => {}, signal, 15),
            e => e instanceof StreamOperationTimeout && e.phase === 'buffer');
        assert.equal(target.active.size, 0);
        await sourceBufferOperation(target, () => target.dispatchEvent(new Event('updateend')), signal, 20);
        assert.equal(target.active.size, 0);
        await assert.rejects(sourceBufferOperation(target, () => { throw new DOMException('full', 'QuotaExceededError') }, signal),
            e => e.name === 'QuotaExceededError');
        assert.equal(target.active.size, 0);
        let called = false;
        await assert.rejects(sourceBufferOperation(target, () => { called = true }, abort.signal), e => e.name === 'AbortError');
        assert.equal(called, false);
    ''')


def test_source_open_has_timeout_and_cancellation(recovery_module):
    run(recovery_module, '''
        const source = Object.assign(new EventTarget(), {readyState:'closed'});
        const controller = new AbortController();
        const pending = waitForSourceOpen(source, controller.signal);
        controller.abort();
        await assert.rejects(pending, e => e.name === 'AbortError');
        await assert.rejects(waitForSourceOpen(source, new AbortController().signal, 15),
            e => e instanceof StreamOperationTimeout && e.phase === 'source-open');
        source.readyState = 'open';
        await waitForSourceOpen(source, new AbortController().signal);
    ''')


def test_read_timeout_cancels_reader_and_abort_rejects(recovery_module):
    run(recovery_module, '''
        let cancelled = false;
        const reader = {read:() => new Promise(() => {}), cancel:async () => {cancelled = true}};
        await assert.rejects(readStreamChunk(reader, 15, new AbortController().signal),
            e => e instanceof StreamOperationTimeout && e.phase === 'read');
        assert(cancelled);
        const controller = new AbortController();
        const pending = readStreamChunk(reader, 1000, controller.signal);
        controller.abort();
        await assert.rejects(pending, e => e.name === 'AbortError');
        const data = {done:false,value:new Uint8Array([1,2])};
        assert.equal(await readStreamChunk({read:async () => data}, 50, new AbortController().signal), data);
    ''')


def test_time_seeks_cannot_hide_a_frozen_video(recovery_module):
    run(recovery_module, '''
        const progress = new PlaybackProgress(0, 0, 0);
        let health;
        for (let t = 250; t <= 16000; t += 250) health = progress.sample(t, t / 1000, 0, false, false);
        assert(health.stalled);
        assert(!health.stable);
        const fallback = new PlaybackProgress(0, 0, null);
        for (let t = 250; t <= 16000; t += 250) health = fallback.sample(t, t / 250, null, false, false);
        assert(health.stalled);
    ''')


def test_only_sustained_presentation_is_stable_and_visibility_resets_it(recovery_module):
    run(recovery_module, '''
        const progress = new PlaybackProgress(0, 0, 0);
        let health;
        for (let t = 250; t <= 10000; t += 250) health = progress.sample(t, t / 1000, t / 25, false, false);
        assert(!health.stable);
        assert(!progress.sample(10100, 10, 400, false, false).stable);
        assert(progress.sample(10250, 10.25, 410, false, false).stable);
        assert(!progress.sample(10500, 11, 415, true, false).stable);
        progress.reset(50000, 15, 500);
        health = progress.sample(50250, 15.25, 510, false, false);
        assert(!health.stable && !health.stalled);
        assert(!progress.sample(52250, 15.25, 510, false, true).stable);
    ''')
