"""Configure the image SDK with video development packages deliberately unavailable."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--cmake", required=True)
    args = parser.parse_args()
    pkg_config = shutil.which("pkg-config")
    if not pkg_config:
        raise RuntimeError("pkg-config is required for this configuration test")
    with tempfile.TemporaryDirectory(prefix="vision-inference-dependencies-") as temporary:
        root = Path(temporary)
        wrapper = root / "pkg-config"
        calls = root / "video-package-requests"
        wrapper.write_text(
            f"#!{sys.executable}\n"
            "import subprocess, sys\n"
            "from pathlib import Path\n"
            "if any('gstreamer' in arg or 'gtk+-' in arg for arg in sys.argv[1:]):\n"
            f"    Path({str(calls)!r}).write_text(' '.join(sys.argv[1:]))\n"
            "    sys.exit(1)\n"
            f"sys.exit(subprocess.call([{pkg_config!r}] + sys.argv[1:]))\n"
        )
        wrapper.chmod(0o755)
        result = subprocess.run(
            [args.cmake, "-S", args.source, "-B", str(root / "build"),
             "-DVISION_INFERENCE_ONLY=ON", "-DCMAKE_BUILD_TYPE=Release",
             f"-DPKG_CONFIG_EXECUTABLE={wrapper}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        if result.returncode or calls.exists():
            raise RuntimeError("Image SDK must configure without video packages:\n" + result.stdout)
    print("Image SDK configures without GStreamer/GTK development packages")


if __name__ == "__main__":
    main()
