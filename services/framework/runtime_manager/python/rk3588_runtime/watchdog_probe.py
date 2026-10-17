"""Read-only watchdog discovery across vendor kernels. Never open /dev/watchdog*."""
import gzip
import os
from pathlib import Path
import re

HARDWARE_DRIVERS = {'dw_wdt', 'gpio-wdt'}
DEVICE_NAME = re.compile(r'watchdog[0-9]+')


def inspect(*, sysfs, devices, manager_fds, device_tree, kernel_config, boot_config, drivers):
    errors = []

    def read(path, optional=True):
        try:
            return path.read_bytes()
        except FileNotFoundError:
            if not optional:
                errors.append(f'文件不存在：{path}')
        except OSError as exc:
            errors.append(f'无法读取 {path}：{exc}')
        return b''

    inventory = []
    try:
        entries = list(sysfs.iterdir())
    except FileNotFoundError:
        entries = []
    except OSError as exc:
        entries = []
        errors.append(f'无法读取看门狗设备列表：{exc}')
    for entry in sorted(entries):
        if not DEVICE_NAME.fullmatch(entry.name):
            continue
        driver = entry / 'device/driver'
        try:
            driver_name = driver.resolve().name if driver.is_symlink() else ''
            node_present = (devices / entry.name).is_char_device()
        except OSError as exc:
            errors.append(f'无法检查 {entry.name}：{exc}')
            driver_name, node_present = '', False
        inventory.append({'device': str(devices / entry.name), 'driver': driver_name,
                          'node_present': node_present,
                          'state': read(entry / 'state').decode(errors='replace').strip() or None,
                          'identity': read(entry / 'identity').decode(errors='replace').strip() or None,
                          'manager_owned': False})

    try:
        for fd in manager_fds.iterdir():
            try:
                target = fd.readlink()
            except FileNotFoundError:  # An unrelated descriptor may close during inspection.
                continue
            if target.parent != devices or not re.fullmatch(r'watchdog(?:[0-9]+)?', target.name):
                continue
            # The legacy misc device /dev/watchdog is the watchdog core's ID 0.
            name = 'watchdog0' if target.name == 'watchdog' else target.name
            for item in inventory:
                if Path(item['device']).name == name:
                    item['manager_owned'] = True
    except FileNotFoundError:
        errors.append('无法读取系统管理进程的设备持有状态')
    except OSError as exc:
        errors.append(f'无法确认系统管理进程持有的看门狗：{exc}')

    result = {'devices': inventory, 'device_tree': [], 'kernel_config': {},
              'registered_drivers': [], 'errors': errors}
    # A running device is stronger evidence than optional vendor diagnostic files.
    if any(item['driver'] in HARDWARE_DRIVERS and (item['node_present'] or item['manager_owned'])
           for item in inventory):
        return result

    for name in sorted(HARDWARE_DRIVERS):
        try:
            if (drivers / name).is_dir():
                result['registered_drivers'].append(name)
        except OSError as exc:
            errors.append(f'无法检查驱动 {name}：{exc}')

    def walk_error(exc):
        if not isinstance(exc, FileNotFoundError):
            errors.append(f'无法读取设备树：{exc}')

    for directory, _, files in os.walk(device_tree, onerror=walk_error):
        node = Path(directory)
        if 'compatible' not in files:
            continue
        compatible = read(node / 'compatible').decode(errors='replace').strip('\0').split('\0')
        if not ('snps,dw-wdt' in compatible or 'linux,wdt-gpio' in compatible
                or 'watchdog' in node.name.lower() or 'wdt' in node.name.lower()):
            continue
        status = read(node / 'status').decode(errors='replace').strip('\0') or 'okay'
        blocked_by = None
        # An enabled child of a disabled bus is also unavailable.
        for parent in (node, *node.parents):
            if parent == device_tree.parent:
                break
            value = read(parent / 'status').decode(errors='replace').strip('\0')
            if value and value not in ('ok', 'okay'):
                blocked_by = str(parent.relative_to(device_tree))
                break
        result['device_tree'].append({'path': str(node.relative_to(device_tree)),
                                      'compatible': compatible, 'status': status,
                                      'disabled_by': blocked_by})

    try:
        if kernel_config.exists():
            with gzip.open(kernel_config, 'rt', errors='replace') as config:
                text = config.read(2 * 1024 * 1024)
        else:
            text = read(boot_config).decode(errors='replace')
        for name in ('WATCHDOG', 'DW_WATCHDOG', 'GPIO_WATCHDOG', 'WATCHDOG_SYSFS', 'SOFT_WATCHDOG'):
            match = re.search(rf'^CONFIG_{name}=([ymn])$', text, re.MULTILINE)
            if match:
                result['kernel_config'][name] = match[1]
            elif re.search(rf'^# CONFIG_{name} is not set$', text, re.MULTILINE):
                result['kernel_config'][name] = 'n'
    except (OSError, EOFError) as exc:
        errors.append(f'无法读取内核配置：{exc}')
    return result


def describe(probe, enabled, manager_error=None):
    """Explain evidence without equating a driver, a setting, or a reset test."""
    inventory = probe['devices']
    hardware = [item for item in inventory if item['driver'] in HARDWARE_DRIVERS]
    available = [item for item in hardware if item['node_present'] or item['manager_owned']]
    disabled = [node for node in probe['device_tree'] if node['disabled_by'] is not None]
    config = probe['kernel_config']
    if enabled:
        state, label = 'active', '已开启'
        summary = '系统管理器正在接管硬件看门狗；停止喂狗后可由硬件触发复位。'
        steps = []
    elif manager_error or probe['errors']:
        state, label = 'inspection_failed', '状态未知'
        summary = '部分系统状态无法读取，目前不能确认整机卡死保护是否生效。'
        steps = ['请管理员检查系统服务和读取权限，再刷新状态。']
    elif available:
        if any(item['manager_owned'] for item in available):
            state, label = 'not_enabled', '尚未启用'
            summary = '系统管理器持有硬件设备，但运行期间的看门狗保护未开启。'
        else:
            state, label = 'not_managed', '尚未接管'
            summary = '已识别硬件看门狗，但系统管理器尚未接管，无法确认整机卡死保护。'
        steps = ['请管理员确认是否已有其他喂狗服务，再通过平台安装器配置系统接管。']
    elif hardware:
        state, label = 'device_node_missing', '设备节点缺失'
        summary = '内核已经注册硬件看门狗，但缺少可用的设备节点。'
        steps = ['请管理员检查 /dev 设备管理和设备节点权限，再配置系统接管。']
    elif config.get('WATCHDOG') == 'n' or (config.get('DW_WATCHDOG') == 'n'
                                          and config.get('GPIO_WATCHDOG') == 'n'):
        state, label = 'driver_missing', '缺少内核驱动'
        summary = '当前内核没有启用所需的硬件看门狗驱动，暂不具备这项整机卡死保护。'
        steps = ['请使用板卡厂商支持看门狗的内核和匹配设备树，再配置系统接管。']
    elif disabled:
        state, label = 'device_tree_disabled', '设备树未开启'
        summary = '检测到看门狗硬件节点被设备树关闭，当前无法通过它保护整机卡死。'
        steps = ['请按板卡厂商说明开启对应设备树节点，更新启动镜像并重启；随后配置系统接管。',
                 '网页不会自动修改设备树或写入启动分区。']
    elif (config.get('DW_WATCHDOG') == 'm' and 'dw_wdt' not in probe['registered_drivers']
          or config.get('GPIO_WATCHDOG') == 'm' and 'gpio-wdt' not in probe['registered_drivers']):
        state, label = 'driver_not_loaded', '驱动模块未加载'
        summary = '内核将硬件看门狗编译为模块，但当前未发现对应驱动加载。'
        steps = ['请管理员检查厂商驱动模块及设备树，完成加载后再配置系统接管。']
    elif inventory and all(item['driver'] == 'softdog' for item in inventory):
        state, label = 'software_only', '仅发现软件看门狗'
        summary = '当前只识别到软件看门狗，不能作为整机严重卡死时的硬件复位保护。'
        steps = ['请检查板卡的硬件看门狗驱动和设备树。']
    elif inventory:
        state, label = 'unsupported_driver', '驱动需确认'
        summary = '发现看门狗设备，但当前无法确认其硬件类型及接管方式。'
        steps = ['请管理员核对厂商驱动；平台不会自动接管未知类型的设备。']
    else:
        state, label = 'device_unavailable', '未发现可用设备'
        summary = '当前没有注册可用的硬件看门狗；仅凭此状态无法判断板卡是否具备相应硬件。'
        steps = ['请管理员检查板卡设备树、驱动绑定和启动日志；驱动已编入不代表设备已启用。']
    return {'state': state, 'label': label, 'summary': summary, 'next_steps': steps,
            'diagnostics': probe}
