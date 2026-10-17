"""All mutations target temporary files, never the board's sysfs."""
import json
import pytest
from rk3588_runtime import performance as p, recovery_history as h


def put(base, name, content):
    path = base / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(content, bytes):
        path.write_bytes(content)
    else:
        path.write_text(str(content))
    return path


@pytest.fixture
def hardware(tmp_path, monkeypatch):
    monkeypatch.setattr(p, 'SYS', tmp_path / 'sys')
    monkeypatch.setattr(p, 'PROC', tmp_path / 'proc')
    put(p.SYS, 'firmware/devicetree/base/model', 'Example RK3588')
    put(p.PROC, 'sys/kernel/osrelease', '5.10-example')
    cpu = p.SYS / 'devices/system/cpu/cpufreq/policy0'
    for name, val in {'related_cpus': '0 1 2 3', 'scaling_cur_freq': 1608000,
                      'scaling_min_freq': 1600000, 'scaling_max_freq': 1704000,
                      'scaling_available_frequencies': '408000 1008000 1608000 1704000',
                      'scaling_governor': 'ondemand'}.items():
        put(cpu, name, val)
    for kind, maximum in [('gpu', 850000000), ('npu', 950000000)]:
        path = p.SYS / f'class/devfreq/example.{kind}'
        for name, val in {'min_freq': 300000000, 'max_freq': maximum, 'cur_freq': maximum,
                          'available_frequencies': f'300000000 600000000 {maximum}',
                          'governor': 'simple_ondemand', 'load': f'100@{maximum}Hz' if kind == 'npu' else '0@300000000Hz'}.items():
            put(path, name, val)
    put(p.SYS, 'kernel/debug/rknpu/load', 'NPU load: Core0: 0%, Core1: 0%, Core2: 0%,')
    zone = p.SYS / 'class/thermal/thermal_zone0'
    put(zone, 'type', 'soc-thermal'); put(zone, 'temp', 40000)
    for n, (kind, temperature) in enumerate([('passive', 75000), ('passive', 85000), ('critical', 115000)]):
        put(zone, f'trip_point_{n}_type', kind)
        put(zone, f'trip_point_{n}_temp', temperature)
        put(zone, f'trip_point_{n}_hyst', 2000)
        dt = p.SYS / f'firmware/devicetree/base/thermal-zones/soc-thermal/trips/trip{n}'
        put(dt, 'type', kind); put(dt, 'temperature', temperature.to_bytes(4, 'big'))
    return cpu, zone


def save(limits):
    return p.save({'revision': p.snapshot()['revision'], 'limits': limits})


def test_read_only_snapshot_prioritizes_npu_driver_and_keeps_zero(hardware, tmp_path):
    before = {f: f.read_bytes() for f in tmp_path.rglob('*') if f.is_file()}
    value = p.snapshot()
    assert len(value['devices']) == 3 and len(value['thermal_trips']) == 3
    assert value['devices'][2]['usage_percent'] == 0
    assert len(value['devices'][2]['core_loads']) == 3
    assert value['devices'][1]['usage_percent'] == 0
    assert value['thermal_trips'][2]['ceiling_celsius'] == 115
    assert value['devices'][0]['max_mhz'] == 1704
    assert not p.PROFILE.exists() and not p.PROFILE.with_suffix('.lock').exists()
    assert before == {f: f.read_bytes() for f in tmp_path.rglob('*') if f.is_file()}


def test_missing_npu_interface_is_unknown_instead_of_generic_100(hardware):
    (p.SYS / 'kernel/debug/rknpu/load').unlink()
    assert p.snapshot()['devices'][2]['usage_percent'] is None
    put(p.PROC, 'rknpu/load', 'NPU load: Core0: 30%, Core1: 60%, Core2: 0%,')
    assert p.snapshot()['devices'][2]['usage_percent'] == 30


def test_frequency_lowering_minimum_order_and_persistent_reapply(hardware, monkeypatch):
    cpu, zone = hardware
    calls = []
    original = p.write_value
    def traced(path, value):
        calls.append((path.name, value)); original(path, value)
    monkeypatch.setattr(p, 'write_value', traced)
    value = save({'CPU:0 1 2 3': 1008, 'thermal:soc-thermal:0': 70})
    assert value['applied'] and value['saved']
    assert calls[0] == ('scaling_max_freq', 1008000)
    assert not any(name == 'scaling_min_freq' for name, _ in calls)
    assert p.PROFILE.stat().st_size < p.MAX_BYTES
    assert list(p.PROFILE.parent.glob('.performance-*')) == []
    put(cpu, 'scaling_min_freq', 1600000); put(cpu, 'scaling_max_freq', 1704000)
    put(zone, 'trip_point_0_temp', 75000)
    assert p.apply_saved()['applied']
    assert p.number(cpu / 'scaling_max_freq') == 1008000
    assert all(not h.is_fault_recovery(event) for event in h.snapshot()['events'])


@pytest.mark.parametrize('limits', [
    {'../../etc/passwd': 1}, {'CPU:0 1 2 3': True}, {'CPU:0 1 2 3': float('nan')},
    {'CPU:0 1 2 3': float('inf')}, {'CPU:0 1 2 3': '1008'}, {'CPU:0 1 2 3': 2500},
    {'CPU:0 1 2 3': 1000}, {'thermal:soc-thermal:2': 116},
    {'thermal:soc-thermal:0': 84}, {'thermal:soc-thermal:0': 40},
    {'thermal:soc-thermal:0': 70.01}, {'thermal:soc-thermal:1': 115},
])
def test_rejects_invalid_values_before_any_hardware_write(hardware, monkeypatch, limits):
    monkeypatch.setattr(p, 'write_value', lambda *args: pytest.fail('invalid settings must not write'))
    with pytest.raises(ValueError):
        save(limits)
    assert not p.PROFILE.exists()


def test_readonly_and_no_dt_critical_are_not_editable(hardware):
    cpu, zone = hardware
    (cpu / 'scaling_max_freq').chmod(0o444)
    for file in (p.SYS / 'firmware/devicetree/base/thermal-zones').rglob('temperature'):
        file.unlink()
    value = p.snapshot()
    assert not value['devices'][0]['editable']
    assert not value['thermal_trips'][2]['editable']
    with pytest.raises(ValueError):
        save({'thermal:soc-thermal:2': 110})


def test_revision_changes_for_limits_not_utilization_or_temperature(hardware):
    cpu, zone = hardware
    old = p.snapshot()['revision']
    put(cpu, 'scaling_cur_freq', 408000); put(zone, 'temp', 42000)
    assert p.snapshot()['revision'] == old
    put(cpu, 'scaling_max_freq', 1608000)
    with pytest.raises(p.PolicyConflict):
        p.save({'revision': old, 'limits': {'CPU:0 1 2 3': 1008}})


def test_kernel_rejection_rolls_back_including_minimum(hardware, monkeypatch):
    cpu, _ = hardware
    original = p.write_value
    def reject(path, value):
        if path.name == 'scaling_max_freq' and value == 1008000:
            raise OSError('kernel rejected')
        original(path, value)
    monkeypatch.setattr(p, 'write_value', reject)
    with pytest.raises(OSError, match='kernel rejected'):
        save({'CPU:0 1 2 3': 1008})
    assert p.number(cpu / 'scaling_min_freq') == 1600000
    assert p.number(cpu / 'scaling_max_freq') == 1704000
    assert not p.PROFILE.exists()


def test_persistence_failure_rolls_back_runtime(hardware, monkeypatch):
    cpu, _ = hardware
    monkeypatch.setattr(p, 'persist', lambda _: (_ for _ in ()).throw(OSError('disk full')))
    with pytest.raises(OSError, match='disk full'):
        save({'CPU:0 1 2 3': 1008})
    assert p.number(cpu / 'scaling_min_freq') == 1600000
    assert p.number(cpu / 'scaling_max_freq') == 1704000


def test_restore_initial_and_board_mismatch_preserves_kernel(hardware, monkeypatch):
    cpu, zone = hardware
    save({'CPU:0 1 2 3': 1008, 'thermal:soc-thermal:0': 70})
    result = p.save({'revision': p.snapshot()['revision']}, True)
    assert result['applied']
    assert p.number(cpu / 'scaling_min_freq') == 1600000
    assert p.number(cpu / 'scaling_max_freq') == 1704000
    assert p.number(zone / 'trip_point_0_temp') == 75000
    put(p.SYS, 'firmware/devicetree/base/model', 'Different vendor carrier')
    assert not p.snapshot()['profile_matches'] and p.snapshot()['notice']
    monkeypatch.setattr(p, 'write_value', lambda *args: pytest.fail('different board must not write'))
    with pytest.raises(ValueError, match='板卡'):
        p.apply_saved()


def test_rollback_failure_is_explicit(hardware, monkeypatch):
    cpu, _ = hardware
    original = p.write_value
    def reject(path, value):
        if path.name == 'scaling_max_freq':
            if value == 1008000:
                path.write_text('1008000')
            raise OSError('write error')
        original(path, value)
    monkeypatch.setattr(p, 'write_value', reject)
    with pytest.raises(RuntimeError, match='回退失败'):
        save({'CPU:0 1 2 3': 1008})
    assert not p.PROFILE.exists()


def test_corrupt_profile_is_visible_and_blocks_mutation(hardware):
    put(p.PROFILE.parent, p.PROFILE.name, '{bad')
    assert p.snapshot()['error']
    with pytest.raises(ValueError):
        save({'CPU:0 1 2 3': 1008})


def test_final_readback_catches_later_kernel_clamp_and_rolls_back(hardware, monkeypatch):
    cpu, _ = hardware
    original = p.write_value
    def clamp(path, value):
        original(path, value)
        if path.name == 'scaling_max_freq' and value == 1008000:
            path.write_text('1200000')
    monkeypatch.setattr(p, 'write_value', clamp)
    with pytest.raises(RuntimeError, match='应用后参数发生变化'):
        save({'CPU:0 1 2 3': 1008})
    assert p.number(cpu / 'scaling_max_freq') == 1704000
    assert p.number(cpu / 'scaling_min_freq') == 1600000
    assert not p.PROFILE.exists()


def test_thermal_thresholds_change_in_safe_order(hardware, monkeypatch):
    original = p.write_value
    calls = []
    def trace(path, value):
        calls.append(path.name); original(path, value)
    monkeypatch.setattr(p, 'write_value', trace)
    save({'thermal:soc-thermal:0': 70, 'thermal:soc-thermal:1': 72})
    assert calls == ['trip_point_0_temp', 'trip_point_1_temp']
    calls.clear()
    save({'thermal:soc-thermal:0': 80, 'thermal:soc-thermal:1': 90})
    assert calls == ['trip_point_1_temp', 'trip_point_0_temp']


def test_saved_revision_cannot_be_reused_for_a_second_write(hardware, monkeypatch):
    previous = p.snapshot()['revision']
    save({'CPU:0 1 2 3': 1008})
    monkeypatch.setattr(p, 'write_value', lambda *args: pytest.fail('stale write must not touch kernel'))
    with pytest.raises(p.PolicyConflict):
        p.save({'revision': previous, 'limits': {'CPU:0 1 2 3': 1704}})


def test_dynamic_cpu_minimum_does_not_invalidate_maximum_frequency_draft(hardware):
    cpu, _ = hardware
    revision = p.snapshot()['revision']
    put(cpu, 'scaling_min_freq', 408000)
    assert p.snapshot()['revision'] == revision
    put(cpu, 'scaling_min_freq', 600000)
    assert p.snapshot()['revision'] == revision
    assert p.save({'revision': revision, 'limits': {'CPU:0 1 2 3': 1008}})['applied']
    assert p.number(cpu / 'scaling_min_freq') == 600000


def test_asynchronous_kernel_readback_is_allowed_a_bounded_settle_interval(tmp_path, monkeypatch):
    path = put(tmp_path, 'scaling_max_freq', 1704000)
    readings = iter([1704000, 1704000, 1008000])
    monkeypatch.setattr(p, 'number', lambda path: next(readings))
    pauses = []
    monkeypatch.setattr(p.time, 'sleep', pauses.append)
    p.write_value(path, 1008000)
    assert pauses == [0.02, 0.02]
    assert path.read_text() == '1008000'
