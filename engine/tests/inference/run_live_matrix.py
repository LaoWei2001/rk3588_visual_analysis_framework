"""Run live camera A/B cases with an active hardware-encoded RTSP output.

The manifest lists name/model_type/model_path/label_path. Copies of the source
config are private. Source settings and system clocks are never modified.
"""
import argparse
import copy
import json
from pathlib import Path
import re
from types import SimpleNamespace

from run_live_capacity import run


def extended_cases(models):
    """Unequal workloads, larger mixtures and per-frame multi-model workloads."""
    by_name = {m['name']: m for m in models}
    def members(names):
        return [by_name[name] for name in names]
    for name, names, copies in (
        ('mixed-light', ('yolov5n', 'yolov8', 'yolov8n_obb'), 3),
        ('mixed-heavy', ('helmet_n1088_1011', 'helmet_n1088_1012', 'diaogou_s_107', 'yolov8_pose'), 3),
        ('mixed-seg-pose-obb', ('yolov5_seg', 'yolov8_pose', 'yolov8n_obb'), 3),
    ):
        assignments = [[by_name[n]] for n in names for _ in range(copies)]
        yield f'{name}-{len(assignments):02d}ch', assignments, False
    skew = [[by_name['yolov5n']]] * 9 + [[by_name['helmet_n1088_1011']]] * 3
    for automatic in (False, True):
        yield 'mixed-skew-12ch-' + ('auto' if automatic else 'pinned'), skew, automatic
    non_depth = [m for m in models if 'depth' not in m['model_type']]
    yield 'mixed-ten-types-15ch-auto', [[non_depth[i % len(non_depth)]] for i in range(15)], True
    for names, count in (
        (('yolov8', 'yolov8_pose', 'yolov8n_obb'), 3),
        (('yolov8', 'yolov5_seg', 'yolov8_pose', 'yolov8n_obb'), 3),
        (('yolov8', 'yolov5_seg', 'yolov8_pose', 'yolov8n_obb'), 6),
    ):
        yield f'composite-{len(names)}models-{count:02d}ch-extended', [members(names)] * count, True
    unequal = [members(('yolov8', 'yolov5n', 'helmet_n1088_1011', 'yolov8_pose', 'yolov5_seg', 'yolov8n_obb')),
               members(('yolov8', 'yolov5n')), members(('yolov5n',))]
    for copies in (1, 2):
        yield f'heterogeneous-621-{3*copies:02d}ch-auto', unequal * copies, True


def cases(models):
    by_name = {m['name']: m for m in models}
    for model in models:
        for count in (3, 15):
            yield f"{model['name']}-{count:02d}ch", [[model]] * count, False
    for name in ('yolov8', 'yolov5n'):
        for count in (1, 6):
            yield f'{name}-{count:02d}ch', [[by_name[name]]] * count, False
    for name, members in (
        ('mixed-det-09ch', ('yolov8', 'helmet_n1088_1011', 'yolov5n')),
        ('mixed-all-15ch', ('yolov8', 'yolov5s', 'yolov8_pose', 'yolov5_seg', 'yolov8n_obb')),
        ('mixed-depth-12ch', ('yolov8', 'yolov8_pose', 'yolov5_seg', 'yolo26n-depth-float')),
    ):
        # Give each core the same mix; otherwise a heavy model can saturate
        # just one core while the other two finish their video frames early.
        yield name, [[by_name[n]] for n in members for _ in range(3)], False
    for members in (('yolov8', 'yolov5n'),
                    ('yolov8', 'yolov5n', 'helmet_n1088_1011', 'yolov8_pose', 'yolov5_seg', 'yolov8n_obb')):
        for count in (1, 3):
            yield f'composite-{len(members)}models-{count:02d}ch', [[by_name[n] for n in members]] * count, True


def main(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    output.chmod(0o700)
    original = json.loads(args.config.read_bytes())
    models = json.loads(args.manifest.read_text())
    index_path = output/'index.json'
    index = json.loads(index_path.read_text()) if index_path.exists() else []
    suite = extended_cases if args.suite == 'extended' else cases
    for number, (name, assignments, automatic) in enumerate(suite(models)):
        if not re.search(args.case, name):
            continue
        config = copy.deepcopy(original)
        if args.threads is not None:
            config['global']['channel_threads'] = args.threads
        template = original['channels'][0]
        config['channels'] = []
        config['_editor_layout'] = {}
        for channel, members in enumerate(assignments):
            row = copy.deepcopy(template)
            row['id'] = channel
            if args.threads is not None:
                row['threads'] = args.threads
            row['models'] = []
            for model in members:
                spec = copy.deepcopy(template['models'][0])
                spec.update({k: model[k] for k in ('model_path', 'label_path', 'model_type')})
                spec.update(id=model['name'], npu_core=-1 if automatic else (1, 0, 2)[channel % 3])
                row['models'].append(spec)
            config['channels'].append(row)
        config_dir = output/'configs'
        config_dir.mkdir(exist_ok=True)
        config_dir.chmod(0o700)
        config_path = config_dir/(name+'.json')
        config_path.write_text(json.dumps(config, indent=2))
        config_path.chmod(0o600)
        variants = args.variants.split(',')
        if number % 2:
            variants.reverse()
        for variant in variants:
            binary = getattr(args, variant)
            if binary is None:
                raise ValueError('Missing --'+variant)
            target = output/(name+'-'+variant)
            if (target/'result.json').exists():
                if json.loads((target/'result.json').read_text()).get('valid'):
                    continue
                raise RuntimeError(f'Previous invalid run needs inspection: {target}')
            result = run(SimpleNamespace(binary=binary, app=args.app, config=config_path,
                output=target, warmup=args.warmup, seconds=args.seconds))
            record = dict(case=name, variant=variant, result=str(target/'result.json'),
                          model_count=sum(len(a) for a in assignments),
                          model_calls_per_second=sum(result['channels'][str(i)]['infer_fps'] * len(a)
                                                     for i, a in enumerate(assignments)))
            index.append(record)
            index_path.write_text(json.dumps(index, indent=2))
            print('FINISHED', name, variant, 'model calls/s', round(record['model_calls_per_second'], 2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('app', 'config', 'manifest', 'output'):
        parser.add_argument('--'+key, type=Path, required=True)
    for key in ('baseline', 'candidate'):
        parser.add_argument('--'+key, type=Path)
    parser.add_argument('--variants', default='baseline,candidate')
    parser.add_argument('--suite', choices=('standard', 'extended'), default='standard')
    parser.add_argument('--threads', type=int, choices=range(1, 9), help='Override per-channel worker count')
    parser.add_argument('--case', default='.')
    parser.add_argument('--warmup', type=float, default=10)
    parser.add_argument('--seconds', type=float, default=20)
    main(parser.parse_args())
