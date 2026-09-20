"""RKVision 命令行的中文参数错误格式化。"""

from __future__ import annotations

import argparse
import re
import sys


def translate_argument_error(message: str) -> str:
    """将 argparse 常见的英文错误转为中文，同时保留参数名和取值。"""
    replacements = (
        (r"^the following arguments are required: (.+)$", r"缺少必填参数：\1"),
        (r"^unrecognized arguments: (.+)$", r"无法识别的参数：\1"),
        (r"^argument (.+): expected one argument$", r"参数 \1 需要一个值"),
        (r"^argument (.+): expected at least one argument$", r"参数 \1 至少需要一个值"),
        (r"^argument (.+): not allowed with argument (.+)$", r"参数 \1 不能与 \2 同时使用"),
        (r"^one of the arguments (.+) is required$", r"必须指定以下参数之一：\1"),
        (r"^ambiguous option: (.+) could match (.+)$", r"参数 \1 存在歧义，可能是：\2"),
    )
    for pattern, replacement in replacements:
        if re.match(pattern, message):
            return re.sub(pattern, replacement, message)

    choice = re.match(
        r"^argument (.+): invalid choice: (.+) \(choose from (.+)\)$", message
    )
    if choice:
        return f"参数 {choice.group(1)} 的值 {choice.group(2)} 无效，可选值：{choice.group(3)}"

    invalid = re.match(r"^argument (.+): invalid (.+) value: (.+)$", message)
    if invalid:
        value_type = {
            "int": "整数",
            "float": "数字",
            "Path": "路径",
        }.get(invalid.group(2), "参数")
        return f"参数 {invalid.group(1)} 的值 {invalid.group(3)} 不是有效的{value_type}"

    # 不直接透传 argparse 的英文文案，避免少见分支回退成英文提示。
    return "参数格式不正确，请根据上方用法检查命令"


class ChineseArgumentParser(argparse.ArgumentParser):
    """使所有 RKVision 主命令和子命令使用中文错误提示。"""

    def format_usage(self) -> str:
        usage = super().format_usage()
        return usage.replace("usage:", "用法：", 1)

    def error(self, message: str) -> None:
        self.print_usage(sys.stderr)
        self.exit(2, f"{self.prog}：错误：{translate_argument_error(message)}\n")
