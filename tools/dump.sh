#!/bin/bash
# Asks the mod for the next eye and UI images; writes build/eyes.png (left | right) and
# build/ui.png (UI over grey so transparency shows).
source "$(dirname "$0")/env.sh"
rm -f "$G/voidvr_left.tga" "$G/voidvr_right.tga" "$G/voidvr_ui.tga"
touch "$G/voidvr_dump.txt"
for i in $(seq 1 30); do sleep 0.5; [ -f "$G/voidvr_ui.tga" ] && break; done
sleep 0.5
python - "$G" "$R/build" "${1:-1600}" <<'PY'
import sys, os
from PIL import Image
g, out, size = sys.argv[1], sys.argv[2], int(sys.argv[3])
eyes = [f"{g}/voidvr_{n}.tga" for n in ("left", "right")]
if all(os.path.exists(p) for p in eyes):
    l, r = [Image.open(p).convert("RGB") for p in eyes]
    o = Image.new("RGB", (l.width + r.width + 10, l.height), (60, 60, 60))
    o.paste(l, (0, 0)); o.paste(r, (l.width + 10, 0))
    print("eyes", l.size)
    o.thumbnail((size, size)); o.save(f"{out}/eyes.png")
else:
    print("missing eyes")
p = f"{g}/voidvr_ui.tga"
if os.path.exists(p):
    im = Image.open(p)
    bg = Image.new("RGBA", im.size, (90, 90, 90, 255))
    im = Image.alpha_composite(bg, im.convert("RGBA")).convert("RGB")
    print("ui", im.size)
    im.thumbnail((size, size)); im.save(f"{out}/ui.png")
PY
