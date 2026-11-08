#!/bin/sh
set -e
cd "$(dirname "$0")"
W=$(mktemp -d)
./mingw.sh "$W" >/dev/null
python3 - "$W" > ../pe_samples_mingw.hpp <<'PY'
import sys, base64
w = sys.argv[1]
print("#pragma once")
print()
items = (("mingw_k32", "k32.exe"), ("mingw_stdio", "stdio-stripped.exe"))
for i, (name, file) in enumerate(items):
    text = base64.b64encode(open(w + "/" + file, "rb").read()).decode()
    print("static const char pe_sample_%s[] =" % name)
    for k in range(0, len(text), 96):
        print('    "%s"' % text[k:k + 96])
    print("    ;")
    if i + 1 < len(items):
        print()
PY
rm -rf "$W"
