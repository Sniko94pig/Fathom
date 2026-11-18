import json, sys
src, out = sys.argv[1], sys.argv[2]
names = sys.argv[3:]
d = json.load(open(src))
lines = ["#pragma once", ""]
for n in names:
    cp = d[n]
    b = chr(cp).encode("utf-8")
    lines.append('#define ICON_%s "%s"' % (n.upper().replace("-", "_"), "".join("\\x%02x" % c for c in b)))
open(out, "w").write("\n".join(lines) + "\n")
