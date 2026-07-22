# 代码格式化工具
import os
import subprocess
from pathlib import Path

def format_cpp_code(directory='.'):
    """
    递归遍历目录，并使用 clang-format 格式化 C/C++ 代码
    """
    # 你想格式化的文件后缀
    extensions = ['.cpp', '.h', '.c', '.hpp']
    target_dir = Path(directory)

    if not target_dir.exists():
        print(f"目录 {directory} 不存在！")
        return

    print(f"正在扫描目录: {target_dir.absolute()}")
    
    formatted_count = 0
    # rglog('*') 会递归遍历所有子目录
    for file_path in target_dir.rglob('*'):
        if file_path.suffix in extensions:
            print(f"正在格式化: {file_path.name}")
            try:
                # -i 参数代表 in-place（就地覆盖保存）
                # -style=file 会优先寻找目录下的 .clang-format 配置文件，找不到则用默认风格
                subprocess.run(
                    ['clang-format', '-i', '-style=file', str(file_path)],
                    check=True,
                    # 隐藏 clang-format 自身的警告输出，保持终端整洁
                    stderr=subprocess.DEVNULL 
                )
                formatted_count += 1
            except subprocess.CalledProcessError as e:
                print(f"格式化失败 {file_path.name}: {e}")
            except FileNotFoundError:
                print("致命错误: 未找到 clang-format！")
                print("请确认是否已安装 (Ubuntu可执行: sudo apt install clang-format)")
                return

    print("-" * 30)
    print(f"批量格式化完成！共处理了 {formatted_count} 个文件。")

if __name__ == "__main__":
    # 默认格式化当前路径下所有文件
    # 你也可以把 '.' 改成你的具体的工程绝对路径，例如 '/userdata/rk3588_visual_analysis_framework'
    format_cpp_code('.')
    