"""Exercise the shared Axios transport without a backend or browser."""
from pathlib import Path
import subprocess


def test_capture_urls_auth_races_and_shared_reads():
    frontend = Path(__file__).resolve().parents[1]
    script = r'''
const esbuild = require('./node_modules/esbuild');
const vm = require('node:vm');
const source = `
import assert from 'node:assert/strict'
import { AxiosError } from 'axios'
import { api, apiErrorMessage, sharedRead } from './src/api/http'
import { useAuthStore } from './src/store/authStore'
import { fetchCaptureStatus, fetchCaptureDevices, fetchCaptureStorage,
  startCapturePreview, stopCapturePreview, startCaptureRecording,
  stopCaptureRecording, videoCaptureErrorMessage } from './src/api/videoCapture'

async function main() {
  useAuthStore.getState().setAuth('old', 'tester')
  const seen = []
  api.defaults.adapter = async config => {
    seen.push([config.method, config.url, config.headers.Authorization])
    return { data: { devices: [] }, status: 200, statusText: 'OK', headers: {}, config }
  }
  const source = { source_type: 'usb', rtsp_url: '', usb_device: '/dev/video0', usb_width: 640, usb_height: 480 }
  await fetchCaptureDevices()
  await fetchCaptureStorage('/tmp')
  await fetchCaptureStatus()
  await startCapturePreview(source)
  await stopCapturePreview()
  await startCaptureRecording(source, 10)
  await stopCaptureRecording()
  assert.deepEqual(seen.map(([method, path]) => [method, path]), [
    ['get', '/video-capture/devices'], ['get', '/video-capture/storage'],
    ['get', '/video-capture/status'], ['post', '/video-capture/preview/start'],
    ['post', '/video-capture/preview/stop'], ['post', '/video-capture/recordings/start'],
    ['post', '/video-capture/recordings/stop'],
  ])
  assert.ok(seen.every(([, , token]) => token === 'Bearer old'))
  assert.equal(videoCaptureErrorMessage, apiErrorMessage)
  assert.equal(videoCaptureErrorMessage(new AxiosError('invalid', '', undefined, undefined,
    { data: { detail: [{ msg: 'bad input' }] } })), 'bad input')

  let rejectRequest
  let started
  const ready = new Promise(resolve => { started = resolve })
  api.defaults.adapter = config => new Promise((resolve, reject) => {
    rejectRequest = () => reject(new AxiosError('unauthorized', '', config, undefined,
      { data: {}, status: 401, statusText: '', headers: {}, config }))
    started()
  })
  const oldRequest = fetchCaptureStatus()
  await ready
  useAuthStore.getState().setAuth('new', 'tester')
  rejectRequest()
  await assert.rejects(oldRequest)
  assert.equal(useAuthStore.getState().token, 'new')
  assert.equal(window.location.href, '/video-capture')

  const currentReady = new Promise(resolve => { started = resolve })
  const currentRequest = fetchCaptureStatus()
  await currentReady
  rejectRequest()
  await assert.rejects(currentRequest)
  assert.equal(useAuthStore.getState().token, null)
  assert.equal(window.location.href, '/login')

  let finish
  let calls = 0
  const read = () => { calls++; return new Promise(resolve => { finish = resolve }) }
  const one = sharedRead('status', read)
  assert.equal(sharedRead('status', read), one)
  assert.equal(calls, 1)
  finish(1)
  await one
  const two = sharedRead('status', read)
  assert.notEqual(two, one)
  assert.equal(calls, 2)
  finish(2)
  await two
}
const timeout = setTimeout(() => { console.error('API test timed out'); process.exit(1) }, 10000)
main().catch(error => { console.error(error); process.exitCode = 1 }).finally(() => clearTimeout(timeout))
`;
const storage = () => {
  const values = new Map();
  return { getItem: key => values.get(key) ?? null, setItem: (key, value) => values.set(key, value),
    removeItem: key => values.delete(key) };
};
global.localStorage = storage();
global.sessionStorage = storage();
Object.defineProperty(global, 'navigator', { value: { userAgent: 'test' }, configurable: true });
global.window = { localStorage, sessionStorage,
  location: { pathname: '/video-capture', href: '/video-capture' } };
const result = esbuild.buildSync({ stdin: { contents: source, resolveDir: process.cwd(), loader: 'ts' },
  bundle: true, platform: 'node', format: 'cjs', write: false });
vm.runInThisContext(result.outputFiles[0].text);
'''
    subprocess.run(['node', '-e', script], cwd=frontend, check=True)
