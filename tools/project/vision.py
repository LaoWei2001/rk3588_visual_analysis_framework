#!/usr/bin/env python3
"""Create, validate, build and package independent RK3588 vision projects."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ENGINE = Path(__file__).resolve().parents[2]
SDK_API = 1
sys.path.insert(0, str(ENGINE / 'vision_analysis/scripts'))
from generate_logics_catalog import build_catalog, ManifestError, atomic_write_json


def run(command, **kwargs):
    print('+ ' + ' '.join(map(str, command)), flush=True)
    subprocess.run([str(arg) for arg in command], check=True, **kwargs)


def read_json(path):
    value = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(value, dict):
        raise ValueError(f'{path}: expected a JSON object')
    return value


def version():
    return (ENGINE / 'VERSION').read_text().strip()


def revision():
    try:
        return subprocess.check_output(['git', '-C', str(ENGINE), 'rev-parse', 'HEAD'], stderr=subprocess.DEVNULL, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return 'source-archive'


def dirty():
    try:
        return bool(subprocess.check_output(['git', '-C', str(ENGINE), 'status', '--porcelain'], stderr=subprocess.DEVNULL, text=True).strip())
    except (OSError, subprocess.CalledProcessError):
        return None


def project_info(project):
    data = read_json(project / 'project.json')
    if not isinstance(data.get('id'), str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]*', data['id']):
        raise ValueError('project.id must contain letters, numbers, underscores or hyphens and start with a letter/number')
    if not isinstance(data.get('version'), str) or not data['version']:
        raise ValueError('project.version is required')
    required = data.get('engine', {})
    if not isinstance(required, dict) or required.get('version') != version():
        raise ValueError(f"Engine version mismatch: project requires {required}, current engine is {version()}")
    if type(required.get('sdk_api')) is not int or required['sdk_api'] != SDK_API:
        raise ValueError(f'Project must declare engine.sdk_api = {SDK_API}')
    if required.get('commit'):
        if required['commit'] != revision():
            raise ValueError('Engine commit does not match project.engine.commit')
        if dirty():
            raise ValueError('A commit-pinned project requires a clean engine working tree')
    cfg = data.get('default_config', 'assets/config.json')
    if not isinstance(cfg, str) or Path(cfg).parent != Path('assets') or not cfg.endswith('.json'):
        raise ValueError('default_config must be a JSON file directly under assets/')
    if not (project / cfg).is_file():
        raise ValueError(f'Missing default configuration: {cfg}')
    # Source ownership is explicit: never build accidental framework copies in logic/.
    if (project / 'logic/core').exists():
        raise ValueError('logic/core belongs to the engine; remove the copied framework directory')
    return data


def check(project):
    data = project_info(project)
    catalog = build_catalog(project / 'logic')
    print(f"Validated {data['id']}: {len(catalog['channel_logics'])} channel, "
          f"{len(catalog['global_logics'])} global modules; engine {version()}, SDK {SDK_API}")
    return data


def create(project):
    if project.exists():
        raise ValueError(f'Destination already exists: {project}')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]*', project.name):
        raise ValueError('Use a project directory name containing letters, numbers, underscores or hyphens')
    shutil.copytree(ENGINE / 'templates/basic_project', project)
    data = {'id': project.name, 'version': '1.0.0', 'engine': {'version': version(), 'sdk_api': SDK_API},
            'default_config': 'assets/config.json'}
    atomic_write_json(project / 'project.json', data)
    (project / 'engine.local.cmake').write_text('set(RKVISION_ENGINE [[' + str(ENGINE) + ']])\n')
    check(project)
    print(f'Created {project}')


def build_path(project, args):
    # Toolchain identity and engine location prevent accidentally reusing another engine cache.
    identity = 'shared-abi1\n' + str(ENGINE) + '\n' + revision() + '\n' + str(Path(args.toolchain).resolve() if args.toolchain else '') + '\n' + str(args.image or '')
    digest = hashlib.sha256(identity.encode()).hexdigest()[:12]
    return project / 'build' / f'{version()}-{digest}-{args.build_type}'


def build(project, args):
    check(project)
    if args.jobs < 1:
        raise ValueError('--jobs must be positive')
    command = ['bash', project / 'build.sh', '--engine', ENGINE,
               '--build-type', args.build_type, '--jobs', args.jobs]
    if args.clean: command.append('--clean')
    if args.toolchain: command.extend(['--toolchain', Path(args.toolchain).resolve()])
    if args.image: command.extend(['--image', args.image])
    run(command)
    return build_path(project, args)


def prepared_build(project, args):
    if not args.build_dir:
        return build(project, args)
    check(project)
    output = args.build_dir.resolve()
    # Only package the exact project/engine/configuration requested.
    cache = (output / 'CMakeCache.txt').read_text()
    def cached(key):
        match = re.search(r'^' + re.escape(key) + r':[^=]*=(.*)$', cache, re.MULTILINE)
        return match.group(1) if match else None
    docker = args.image or (platform.machine() not in ('aarch64', 'armv7l') and not args.toolchain)
    expected_project, expected_engine = ('/project', '/engine') if docker else (str(project), str(ENGINE))
    if cached('CMAKE_HOME_DIRECTORY') != expected_project or cached('RKVISION_ENGINE') != expected_engine or cached('CMAKE_BUILD_TYPE') != args.build_type:
        raise ValueError('Build directory belongs to a different project, engine or build type')
    if not (output / 'vision_analysis').is_file():
        raise ValueError('Build the application before packaging')
    return output


def copy_tree(source, destination):
    if source.is_dir():
        shutil.copytree(source, destination, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '.pytest_cache', 'tests', 'logs', '*.log'))


def elf_dependencies(*binaries):
    # readelf translates "Shared library" under Chinese locales too.
    environment = dict(os.environ, LC_ALL='C', LANGUAGE='C')
    dynamic = subprocess.check_output(['readelf', '-d', *map(str, binaries)],
                                      text=True, env=environment)
    return sorted(set(re.findall(r'Shared library: \[(.*?)\]', dynamic)))


def package(project, args):
    build_dir = prepared_build(project, args)
    data = project_info(project)
    destination = Path(args.output).resolve() if args.output else project / 'dist' / data['id']
    # Never replace project source, engine source or a parent containing them.
    protected = [project, ENGINE, project / 'logic', project / 'assets', project / 'report_templates', project / 'build']
    if any(destination == p or destination in p.parents or p in destination.parents for p in protected[2:]) or destination in (project, ENGINE) or destination in project.parents or destination in ENGINE.parents:
        raise ValueError('Package output must not replace source or build directories')
    if destination.exists() and not (destination / 'app.json').is_file():
        raise ValueError(f'Refusing to replace an unrecognized directory: {destination}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.vision-package-', dir=destination.parent))
    archive = destination.with_name(destination.name + '.tar.gz')
    archive_temp = staging.with_name(staging.name + '.tar.gz')
    try:
        shutil.copy2(build_dir / 'vision_analysis', staging / 'vision_analysis')
        copy_tree(project / 'assets', staging / 'assets')
        if data['default_config'] != 'assets/config.json' and not (staging / 'assets/config.json').exists():
            shutil.copy2(project / data['default_config'], staging / 'assets/config.json')
        (staging / 'run.config').write_text(Path(data['default_config']).name + '\n')
        atomic_write_json(staging / 'logics.json', build_catalog(project / 'logic'))
        for name in ('upload', 'model_update'):
            copy_tree(ENGINE / 'service' / name, staging / 'services' / name)
        run([sys.executable, ENGINE / 'vision_analysis/scripts/generate_report_templates.py',
             '--logic-root', project / 'logic', '--app-dir', project / 'report_templates',
             '--adapter-catalog', ENGINE / 'service/upload/adapters/catalog.json', '--output', staging / 'report_templates'])
        # Pin and verify the same RKNN runtime as CMake, including on cross-build hosts.
        vendor = ENGINE / 'vision_analysis/vendor/rknn/2.4.2a2'
        (staging / 'libs').mkdir()
        # The engine is part of every runnable app, even with --no-bundle-libs.
        engine_runtime = staging / 'libs/librkvision.so.1'
        shutil.copy2(build_dir / 'libs/librkvision.so.1', engine_runtime)
        shutil.copy2(build_dir / 'libs/rkvision-runtime.cmake', staging / 'libs/rkvision-runtime.cmake')
        if not args.no_bundle_libs:
            expected = 'bf50d51705ae433013927a13520ae781b534fdb1481c47bdddbc726f63ed4970'
            runtime = vendor / 'lib/aarch64/librknnrt.so'
            if hashlib.sha256(runtime.read_bytes()).hexdigest() != expected:
                raise ValueError('RKNN runtime checksum mismatch')
            shutil.copy2(runtime, staging / 'libs/librknnrt.so')
            shutil.copy2(vendor / 'RUNTIME_MANIFEST.txt', staging / 'libs/librknnrt.manifest')
            if platform.machine() in ('aarch64', 'armv7l') and not args.toolchain and not args.image:
                # Preserve the existing direct dependency bundling policy; OS baseline supplies libc, etc.
                needed = elf_dependencies(staging / 'vision_analysis', engine_runtime)
                cache = subprocess.check_output(['ldconfig', '-p'], text=True)
                for name in needed:
                    if name.startswith(('ld-linux', 'libc.so', 'libm.so', 'libdl.so', 'librt.so', 'libpthread.so', 'libstdc++.so', 'librknnrt.so', 'librkvision.so')):
                        continue
                    candidates = [line.split('=>')[-1].strip() for line in cache.splitlines() if line.strip().startswith(name + ' ')]
                    if not candidates or not Path(candidates[0]).is_file():
                        raise ValueError(f'Missing runtime dependency: {name}')
                    shutil.copy2(candidates[0], staging / 'libs' / name)
            elif args.image or not args.toolchain:
                image = args.image or 'rk3588_builder:2026_4_30'
                run(['docker', 'run', '--rm', '-e', 'LC_ALL=C', '-e', 'LANGUAGE=C', '-v', f'{staging}:/package', image, 'python3', '-c',
                     'import pathlib,re,shutil,subprocess; '
                     'names=re.findall(r"Shared library: \\[(.*?)\\]",subprocess.check_output(["aarch64-linux-gnu-readelf","-d","/package/vision_analysis","/package/libs/librkvision.so.1"],text=True)); '
                     'skip=("ld-linux","libc.so","libm.so","libdl.so","librt.so","libpthread.so","libstdc++.so","librknnrt.so","librkvision.so"); '
                     '[(shutil.copy2(next(pathlib.Path("/sysroot").rglob(n)),pathlib.Path("/package/libs")/n)) for n in names if not n.startswith(skip)]'])
        if not args.no_strip:
            if args.image or (platform.machine() not in ('aarch64', 'armv7l') and not args.toolchain):
                run(['docker', 'run', '--rm', '-v', f'{staging}:/package', args.image or 'rk3588_builder:2026_4_30',
                     'aarch64-linux-gnu-strip', '/package/vision_analysis', '/package/libs/librkvision.so.1'])
            else:
                run(['aarch64-linux-gnu-strip' if args.toolchain else 'strip', staging / 'vision_analysis', engine_runtime])
        copy_tree(ENGINE / 'tools/project/package_scripts', staging)
        atomic_write_json(staging / 'app.json', {'id': data['id'], 'version': data['version'],
            'engine': {'version': version(), 'sdk_api': SDK_API, 'commit': revision(), 'dirty': dirty(),
                       'runtime_abi': 1, 'runtime_library': 'libs/librkvision.so.1',
                       'runtime_sha256': hashlib.sha256(engine_runtime.read_bytes()).hexdigest()},
            'build_type': args.build_type, 'default_config': data['default_config']})
        with tarfile.open(archive_temp, 'w:gz') as handle:
            handle.add(staging, arcname=data['id'])
        # Replace only a generated package; preserve the last successful output on failure.
        backup = destination.with_name('.' + destination.name + '.previous')
        if backup.exists():
            raise ValueError(f'Previous package backup exists: {backup}')
        if destination.exists():
            destination.rename(backup)
        try:
            staging.rename(destination)
            os.replace(archive_temp, archive)
        except BaseException:
            if destination.exists():
                shutil.rmtree(destination)
            if backup.exists(): backup.rename(destination)
            raise
        if backup.exists(): shutil.rmtree(backup)
        print(f'Packaged {destination}\nArchive: {archive}')
    finally:
        if staging.exists(): shutil.rmtree(staging)
        archive_temp.unlink(missing_ok=True)
    return destination


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    # Share one user-facing command; each workflow keeps its own argument parser.
    entries = {
        'install': ENGINE / 'tools/project/install_app.py',
        'develop': ENGINE / 'docs/skills/rk3588-feature-wizard/scripts/start_wizard.py',
    }
    if argv and argv[0] in entries:
        return subprocess.call([sys.executable, str(entries[argv[0]]), *argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('create', 'check', 'build', 'run', 'package', 'new-logic'):
        p = sub.add_parser(command)
        p.add_argument('project', type=Path)
        if command == 'new-logic':
            p.add_argument('kind', choices=['channel', 'global'])
            p.add_argument('name')
        if command in ('build', 'run', 'package'):
            p.add_argument('--build-type', choices=['Release', 'Debug', 'RelWithDebInfo'], default='Release')
            p.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 4))
            p.add_argument('--clean', action='store_true')
            compiler = p.add_mutually_exclusive_group()
            compiler.add_argument('--toolchain', type=Path)
            compiler.add_argument('--image')
        if command == 'run':
            p.add_argument('--config', type=Path, help='configuration relative to the application root')
        if command == 'package':
            p.add_argument('--build-dir', type=Path, help='package an existing matching CMake build')
            p.add_argument('--output', type=Path)
            p.add_argument('--no-strip', action='store_true')
            p.add_argument('--no-bundle-libs', action='store_true')
    sub.add_parser('install', help='install an application package; use install --help for options')
    sub.add_parser('develop', help='develop application logic with the isolated wizard; use develop --help')
    args = parser.parse_args(argv)
    project = args.project.resolve()
    try:
        if args.command == 'create': create(project)
        elif args.command == 'check': check(project)
        elif args.command == 'new-logic':
            project_info(project)
            run([sys.executable, ENGINE / 'vision_analysis/scripts/create_logic_module.py', args.kind, args.name,
                 '--project-root', project])
            check(project)
        elif args.command == 'build': build(project, args)
        elif args.command == 'run':
            if platform.machine() not in ('aarch64', 'armv7l') or args.toolchain or args.image:
                raise ValueError('Run requires native RK3588; build/package on other hosts and deploy to the device')
            output = build(project, args)
            config = args.config or Path(project_info(project)['default_config'])
            environment = dict(os.environ)
            environment['ASSETS_DIR'] = str(project / 'assets')
            runtime = ENGINE / 'vision_analysis/vendor/rknn/2.4.2a2/lib/aarch64'
            environment['LD_LIBRARY_PATH'] = str(runtime) + (':' + environment['LD_LIBRARY_PATH'] if environment.get('LD_LIBRARY_PATH') else '')
            run([output / 'vision_analysis', config], cwd=project, env=environment)
        elif args.command == 'package': package(project, args)
        return 0
    except (ValueError, OSError, ManifestError, subprocess.CalledProcessError) as exc:
        print(f'Error: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
