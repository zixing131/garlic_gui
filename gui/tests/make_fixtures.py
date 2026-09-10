#!/usr/bin/env python3
"""Generate small, owned JVM/DEX/APK fixtures (JDK; optional Android SDK d8)."""
import argparse
from pathlib import Path
import shutil
import subprocess
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("--d8", help="Android SDK build-tools d8 executable")
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
classes = out / "classes"
classes.mkdir(exist_ok=True)
sources = sorted((Path(__file__).parent / "fixtures").rglob("*.java"))
subprocess.run(["javac", "--release", "8", "-g", "-d", str(classes), *map(str, sources)], check=True)
with zipfile.ZipFile(out / "demo.jar", "w") as jar:
    for path in classes.rglob("*.class"):
        jar.write(path, path.relative_to(classes).as_posix())
shutil.copyfile(classes / "demo/Main.class", out / "Main.class")
(out / "invalid.apk").write_bytes(b"not a valid archive")
if args.d8:
    subprocess.run([args.d8, "--output", str(out), str(out / "demo.jar")], check=True)
    with zipfile.ZipFile(out / "示例 app.apk", "w") as apk:
        apk.write(out / "classes.dex", "classes.dex")
print(out)
