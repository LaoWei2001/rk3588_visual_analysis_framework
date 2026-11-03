"""Run the real CLI against a controllable SDK; no NPU or system frequency writes."""
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time

import pytest

TOOL = Path(__file__).resolve().parents[1]
ROOT = TOOL.parents[2]
SDK = ROOT / "engine/vendor/rknn/2.4.2a2/include"


@pytest.fixture(scope="session")
def program(tmp_path_factory):
    directory = tmp_path_factory.mktemp("npu-bench-sdk")
    library = directory / "libfake_rknn.so"
    subprocess.run([
        "g++", "-shared", "-fPIC", "-std=c++14", "-pthread", "-I", str(SDK),
        str(TOOL / "tests/fake_rknn.cpp"), "-o", str(library),
    ], check=True, capture_output=True, text=True)
    binary = directory / "npu_bench"
    subprocess.run([
        "g++", "-O2", "-std=c++14", "-Wall", "-Wextra", "-pthread",
        "-I", str(SDK), "-I", str(ROOT / "engine/src"),
        f'-DNPU_BENCH_LOCK_PATH="{directory / "owner.lock"}"',
        str(TOOL / "npu_bench.cpp"), str(library), "-lcrypto", "-ldl",
        f"-Wl,-rpath,{directory}", "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    return binary


def run(program, tmp_path, args=(), env=None, expect=0):
    model = tmp_path / 'model "example".rknn'
    model.write_bytes(b"fake-model")
    report = tmp_path / "report.json"
    trace = tmp_path / "trace.log"
    environment = dict(os.environ, FAKE_TRACE=str(trace), **(env or {}))
    command = [
        str(program), str(model), "--warmup", ".025", "--seconds", ".10",
        "--repeats", "1", "--json", str(report), *args,
    ]
    result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=10)
    assert result.returncode == expect, result.stdout + result.stderr
    data = json.loads(report.read_text()) if report.exists() else None
    return result, data, trace.read_text().splitlines() if trace.exists() else []


def test_native_io_and_report(program, tmp_path):
    result, report, trace = run(program, tmp_path, env={"FAKE_SCENARIO": "multi"})
    assert report["valid"] is True
    assert report["config"]["profiling"] is False
    assert report["model"]["sha256"] == hashlib.sha256(b"fake-model").hexdigest()
    assert len(report["inputs"]) == len(report["outputs"]) == 2
    assert report["inputs"][0]["type"] == "FP16"
    assert report["inputs"][1]["type"] == "INT64"
    assert report["outputs"][0]["layout"] == "NC1HWC2"
    assert "get" not in trace
    assert trace.count("free") == sum(line.startswith("alloc ") for line in trace)
    metrics = report["rounds"][0]["workers"][0]["metrics"]
    assert metrics["completed"] > 0 and metrics["warmup_completed"] > 0
    assert metrics["mean_readback_ms"] == 0
    assert metrics["mean_ms"] == metrics["mean_run_ms"]
    assert report["summary"]["inferences_per_second"] == pytest.approx(metrics["completed"] / .1)


def test_warmup_excludes_slow_first_call_and_initialization(program, tmp_path):
    _, report, _ = run(program, tmp_path, args=("--warmup", ".08"), env={
        "FAKE_INIT_US": "100000", "FAKE_FIRST_RUN_US": "50000", "FAKE_RUN_US": "5000"})
    metrics = report["rounds"][0]["workers"][0]["metrics"]
    assert metrics["warmup_completed"] >= 2
    assert metrics["mean_ms"] < 30
    assert report["rounds"][0]["window_seconds"] == .1


def test_same_core_submission_has_no_framework_gate(program, tmp_path):
    _, report, trace = run(program, tmp_path, args=(
        "--mode", "throughput", "--instances", "3", "--cores", "0"))
    assert "overlap" in trace
    assert report["config"]["core_masks"] == [1, 1, 1]
    assert len(report["rounds"][0]["workers"]) == 3


def test_readback_is_separate_and_included(program, tmp_path):
    _, report, trace = run(program, tmp_path, args=("--io", "readback"))
    metrics = report["rounds"][0]["workers"][0]["metrics"]
    assert metrics["mean_readback_ms"] >= 1
    assert metrics["mean_ms"] == pytest.approx(metrics["mean_run_ms"] + metrics["mean_readback_ms"])
    assert trace.count("get") == trace.count("release")
    assert report["outputs"][0]["layout"] == "NCHW"


def test_deadline_tail_not_counted(program, tmp_path):
    _, report, _ = run(program, tmp_path, args=("--warmup", "0", "--seconds", ".10"),
                       env={"FAKE_RUN_US": "60000"})
    metrics = report["rounds"][0]["workers"][0]["metrics"]
    assert metrics["completed"] == 1
    assert metrics["outside_window_completed"] == 1
    assert report["rounds"][0]["inferences_per_second"] == 10
    assert report["rounds"][0]["drain_ms"] >= 10


def test_sampling_and_repeats(program, tmp_path):
    _, report, _ = run(program, tmp_path, args=("--max-samples", "2", "--repeats", "2"))
    assert len(report["rounds"]) == 2
    for round_ in report["rounds"]:
        metrics = round_["workers"][0]["metrics"]
        assert metrics["latency_samples"] == 2 and metrics["percentiles_sampled"]
        assert metrics["p50_ms"] <= metrics["p95_ms"]
    assert report["summary"]["completed"] == sum(r["completed"] for r in report["rounds"])


@pytest.mark.parametrize("failure", [
    "init", "core", "query", "native_query", "create", "create2", "bind",
    "run", "run_later", "free", "destroy",
])
def test_failed_run_is_invalid_and_resources_released(program, tmp_path, failure):
    result, report, trace = run(program, tmp_path, env={"FAKE_FAIL": failure}, expect=1)
    assert report["valid"] is False and report["error"]
    assert "测试有效：" not in result.stdout
    assert trace.count("free") == sum(line.startswith("alloc ") for line in trace)


@pytest.mark.parametrize("failure", ["get", "release"])
def test_failed_readback_is_invalid(program, tmp_path, failure):
    _, report, _ = run(program, tmp_path, args=("--io", "readback"),
                       env={"FAKE_FAIL": failure}, expect=1)
    assert report["valid"] is False


@pytest.mark.parametrize("args", [
    ("--instances", "0"), ("--instances", "2"), ("--seconds", "nan"),
    ("--seconds", "0"), ("--seconds", "1junk"), ("--cores", "8"),
    ("--io", "unknown"), ("--mode", "unknown"), ("3", "15", "0", "1"),
])
def test_invalid_arguments_do_not_initialize_runtime(program, tmp_path, args):
    _, _, trace = run(program, tmp_path, args=args, expect=1)
    assert "init" not in trace


def test_dynamic_shape_required_and_applied(program, tmp_path):
    _, invalid, _ = run(program, tmp_path, env={"FAKE_SCENARIO": "dynamic"}, expect=1)
    assert "needs --shape" in invalid["error"]
    _, report, _ = run(program, tmp_path, args=("--shape", "0:1x4x4x3"),
                       env={"FAKE_SCENARIO": "dynamic"})
    assert report["inputs"][0]["shape"] == [1, 4, 4, 3]


def test_unverified_dynamic_native_shape_rejected(program, tmp_path):
    _, report, _ = run(program, tmp_path, args=("--shape", "0:1x4x4x3"),
                       env={"FAKE_SCENARIO": "stale"}, expect=1)
    assert "cannot verify" in report["error"]


def test_native_input_file_including_padding(program, tmp_path):
    tensor = tmp_path / "input raw.bin"
    tensor.write_bytes(bytes([127]) * 32)
    _, report, _ = run(program, tmp_path, args=("--input", f"0:{tensor}"),
                       env={"FAKE_INPUT_BYTE": "127"})
    assert report["config"]["input_files"][0]["sha256"] == hashlib.sha256(tensor.read_bytes()).hexdigest()
    tensor.write_bytes(b"too short")
    _, report, _ = run(program, tmp_path, args=("--input", f"0:{tensor}"), expect=1)
    assert "exactly 32 bytes" in report["error"]


def test_report_cannot_overwrite_model(program, tmp_path):
    model = tmp_path / "protected.rknn"
    model.write_bytes(b"keep-model")
    result = subprocess.run([str(program), str(model), "--json", str(model)], capture_output=True, text=True)
    assert result.returncode == 1 and model.read_bytes() == b"keep-model"


def test_sigint_marks_report_invalid(program, tmp_path):
    model = tmp_path / "model.rknn"
    model.write_bytes(b"fake-model")
    report = tmp_path / "signal.json"
    process = subprocess.Popen([
        str(program), str(model), "--warmup", "0", "--seconds", "10",
        "--repeats", "1", "--json", str(report),
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if report.exists() and report.stat().st_size:
                break
            time.sleep(.01)
        time.sleep(.1)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 130, stdout + stderr
        assert json.loads(report.read_text())["valid"] is False
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def test_no_completion_is_not_reported_as_valid_zero_fps(program, tmp_path):
    _, report, _ = run(program, tmp_path, args=("--warmup", "0", "--seconds", ".03"),
                       env={"FAKE_RUN_US": "80000"}, expect=1)
    assert "No completed inference" in report["error"]


@pytest.mark.parametrize("cores, masks", [
    ("auto", [0, 0, 0, 0]), ("round-robin", [1, 2, 4, 1]), ("0,1,2", [7, 7, 7, 7]),
])
def test_requested_core_masks_are_recorded(program, tmp_path, cores, masks):
    _, report, _ = run(program, tmp_path, args=(
        "--mode", "throughput", "--instances", "4", "--cores", cores))
    assert report["config"]["core_masks"] == masks


def test_input_bytes_cached_before_instance_initialization(program, tmp_path):
    model = tmp_path / "model.rknn"
    model.write_bytes(b"fake-model")
    tensor = tmp_path / "input.bin"
    original = bytes([127]) * 32
    tensor.write_bytes(original)
    report = tmp_path / "cached-input.json"
    trace = tmp_path / "cached-input.log"
    process = subprocess.Popen([
        str(program), str(model), "--mode", "throughput", "--instances", "2",
        "--input", f"0:{tensor}", "--warmup", ".01", "--seconds", ".10",
        "--repeats", "1", "--json", str(report),
    ], env=dict(os.environ, FAKE_TRACE=str(trace), FAKE_INIT_US="150000", FAKE_INPUT_BYTE="127"),
       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if trace.exists() and "init" in trace.read_text():
                break
            time.sleep(.005)
        else:
            pytest.fail("Runtime initialization did not start")
        tensor.write_bytes(bytes([0]) * 32)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, stdout + stderr
        data = json.loads(report.read_text())
        assert data["config"]["input_files"][0]["sha256"] == hashlib.sha256(original).hexdigest()
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
