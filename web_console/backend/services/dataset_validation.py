"""Validate saved collection rules without a running engine.

Keep the rule grammar aligned with engine/src/dataset/rules.cpp. The engine
still validates against its actual runtime configuration when synchronizing.
"""
import math

from fastapi import HTTPException


def validate_rules(rules: list, labels: list[str], allowed: list[str], floor: float) -> None:
    def reject(message):
        raise HTTPException(409, message)

    def keys(value, names):
        if not isinstance(value, dict) or set(value) - set(names):
            reject('条件包含未知字段或不是对象')

    def number(value, low, high, integer=False):
        return (type(value) in (int, float) and math.isfinite(value)
                and low <= value <= high and (not integer or value == int(value)))

    def operator(value):
        if value not in ('==', '!=', '>', '>=', '<', '<='):
            reject('无效的数量比较符')

    def count(value):
        keys(value, ('op', 'value'))
        operator(value.get('op'))
        if not number(value.get('value'), 0, 10000, integer=True):
            reject('数量必须是 0 至 10000 的整数')

    def target(value, strict=True):
        if strict:
            keys(value, ('class', 'min_confidence'))
        label = value.get('class')
        if not isinstance(label, str) or not label or label not in labels:
            reject('类别名称必须与该通道的模型标签一致')
        if allowed and label not in allowed:
            reject('模型的检测类别过滤未包含：' + label)
        confidence = value.get('min_confidence', .3)
        if not number(confidence, 0, 1) or confidence + 1e-6 < floor:
            reject('条件置信度必须在模型输出阈值至 1 之间')

    nodes = 0

    def condition(value, depth=0):
        nonlocal nodes
        nodes += 1
        if depth > 8 or nodes > 256:
            reject('条件过于复杂：最多 8 层、256 个条件节点')
        keys(value, ('all', 'any', 'not', 'class', 'min_confidence', 'count', 'compare_counts', 'association'))
        for group in ('all', 'any', 'not', 'compare_counts', 'association'):
            if group not in value:
                continue
            if len(value) != 1:
                reject('条件组只能包含一种条件')
            child = value[group]
            if group == 'not':
                condition(child, depth + 1)
            elif group in ('all', 'any'):
                if not isinstance(child, list) or not child:
                    reject('条件组不能为空')
                for item in child:
                    condition(item, depth + 1)
            elif group == 'compare_counts':
                keys(child, ('left', 'right', 'op'))
                target(child.get('left'))
                target(child.get('right'))
                operator(child.get('op'))
            else:
                keys(child, ('subject', 'object', 'objects', 'relation', 'state', 'quantifier', 'count',
                             'one_to_one', 'min_overlap', 'max_distance', 'object_count'))
                target(child.get('subject'))
                if ('object' in child) == ('objects' in child):
                    reject('关联判断必须填写 object 或 objects 中的一项')
                if 'object' in child:
                    target(child['object'])
                else:
                    objects = child['objects']
                    if not isinstance(objects, list) or not 1 <= len(objects) <= 32:
                        reject('关联类别必须是包含 1 至 32 项的数组')
                    seen = set()
                    for item in objects:
                        target(item)
                        if item['class'] in seen:
                            reject('关联类别不能重复：' + item['class'])
                        seen.add(item['class'])
                if child.get('relation') not in ('center_inside', 'subject_center_inside', 'overlap', 'subject_overlap', 'near'):
                    reject('无效的空间匹配方式')
                if child.get('state') not in ('matched', 'unmatched', 'count'):
                    reject('无效的关联匹配状态')
                if child['state'] == 'count':
                    count(child.get('object_count'))
                    if 'one_to_one' in child:
                        reject('按关联目标数量判断时，不使用一对一分配')
                elif 'object_count' in child:
                    reject('只有按关联目标数量判断时可设置 object_count')
                if child.get('quantifier', 'any') not in ('any', 'all', 'count'):
                    reject('无效的关联目标数量要求')
                if 'count' in child:
                    count(child['count'])
                elif child.get('quantifier') == 'count':
                    reject('按数量判断关联时必须填写 count')
                if 'one_to_one' in child and type(child['one_to_one']) is not bool:
                    reject('one_to_one 必须是布尔值')
                for name, maximum in (('min_overlap', 1), ('max_distance', 100)):
                    if name in child and (not number(child[name], 0, maximum) or child[name] == 0):
                        reject('匹配阈值超出有效范围：' + name)
            return
        target(value, strict=False)
        count(value.get('count'))

    if not isinstance(rules, list) or not 1 <= len(rules) <= 32:
        reject('采集规则必须是包含 1 至 32 条规则的数组')
    ids = set()
    for rule in rules:
        keys(rule, ('id', 'name', 'condition'))
        for name, maximum in (('id', 128), ('name', 256)):
            value = rule.get(name)
            if not isinstance(value, str) or not value or len(value.encode()) > maximum:
                reject('规则 ID 和名称不能为空或过长')
        if rule['id'] in ids:
            reject('规则 ID 必须唯一')
        ids.add(rule['id'])
        condition(rule.get('condition'))
