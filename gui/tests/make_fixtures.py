#!/usr/bin/env python3
"""Generate small, owned JVM/DEX/APK fixtures (JDK; optional Android SDK d8)."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("--d8", help="Android SDK build-tools d8 executable")
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
shutil.copyfile(Path(__file__).parent / 'fixtures/flattened.dex', out / 'flattened.dex')
classes = out / "classes"
classes.mkdir(exist_ok=True)
sources = sorted((Path(__file__).parent / "fixtures").rglob("*.java"))
subprocess.run(["javac", "--release", "8", "-g", "-d", str(classes), *map(str, sources)], check=True)
# A legal JVM stack loop joins at dup with the same value still on the stack.
# This used to cast the dup's empty expression to an assignment and crash.
u2 = lambda n: struct.pack(">H", n)
u4 = lambda n: struct.pack(">I", n)
utf8 = lambda value: b"\x01" + u2(len(value)) + value
pool = [utf8(b"demo/DupJoin"), b"\x07"+u2(1), utf8(b"java/lang/Object"),
        b"\x07"+u2(3), utf8(b"loop"), utf8(b"()Ljava/lang/Object;"), utf8(b"Code")]
code = bytes.fromhex("01 59 c6 ff ff b0")
body = u2(2)+u2(0)+u4(len(code))+code+u2(0)+u2(0)
method = u2(9)+u2(5)+u2(6)+u2(1)+u2(7)+u4(len(body))+body
(classes / "demo/DupJoin.class").write_bytes(
    bytes.fromhex("cafebabe")+u2(0)+u2(49)+u2(8)+b"".join(pool)+
    u2(0x21)+u2(2)+u2(4)+u2(0)+u2(0)+u2(1)+method+u2(0))
with zipfile.ZipFile(out / "demo.jar", "w") as jar:
    jar.writestr("layout/main.xml", '<layout><text>中文 resource preview</text></layout>')
    for path in classes.rglob("*.class"):
        jar.write(path, path.relative_to(classes).as_posix())
with zipfile.ZipFile(out / "demo.zip", "w") as archive:
    archive.writestr("assets/readme.txt", "ZIP resource preview")
    for path in classes.rglob("*.class"):
        archive.write(path, path.relative_to(classes).as_posix())
with zipfile.ZipFile(out / "empty.zip", "w") as archive:
    archive.writestr("assets/readme.txt", "No bytecode here")
# Unknown class attributes are legal JVM extensions. Put one before SourceFile
# so ignoring it without advancing the memory cursor corrupts the next header.
def with_unknown_attribute(data):
    data = bytearray(data)
    u2 = lambda off: struct.unpack_from(">H", data, off)[0]
    u4 = lambda off: struct.unpack_from(">I", data, off)[0]
    off, number, unknown_name = 10, 1, None
    while number < u2(8):
        tag = data[off]; off += 1
        if tag == 1:
            length = u2(off)
            if data[off+2:off+2+length] == b"demo/Main": unknown_name = number
            off += 2 + length
        else:
            off += {3:4,4:4,5:8,6:8,7:2,8:2,9:4,10:4,11:4,12:4,15:3,16:2,18:4}[tag]
            if tag in (5, 6): number += 1
        number += 1
    off += 6
    off += 2 + 2*u2(off)
    for _ in range(2):
        count = u2(off); off += 2
        for _ in range(count):
            attributes = u2(off+6); off += 8
            for _ in range(attributes): off += 6 + u4(off+2)
    count = u2(off)
    assert count and unknown_name
    data[off:off+2] = struct.pack(">H", count+1)
    data[off+2:off+2] = struct.pack(">HI", unknown_name, 4) + b"test"
    return data
with zipfile.ZipFile(out / "unknown.jar", "w") as jar:
    for path in classes.rglob("*.class"):
        content = path.read_bytes()
        if path.relative_to(classes).as_posix() == "demo/Main.class":
            content = with_unknown_attribute(content)
        jar.writestr(path.relative_to(classes).as_posix(), content)
shutil.copyfile(classes / "demo/Main.class", out / "Main.class")
(out / "invalid.apk").write_bytes(b"not a valid archive")
if args.d8:
    subprocess.run([args.d8, "--output", str(out), str(out / "demo.jar")], check=True)
    with zipfile.ZipFile(out / "示例 app.apk", "w") as apk:
        apk.write(out / "classes.dex", "classes.dex")
    with zipfile.ZipFile(out / "nested.apks", "w") as archive:
        archive.write(out / "示例 app.apk", "splits/base-master.apk")
print(out)
