#!/bin/sh
# Regenerate kernel/sample_gif.h (the ClassicTube demo video). Needs ImageMagick.
set -e
cd "$(dirname "$0")/.."
convert -size 96x60 -delay 7 -loop 0 \
  \( -size 96x60 plasma:fractal -blur 0x2 \) \
  \( -clone 0 -roll +12+0 \) \( -clone 0 -roll +24+0 \) \( -clone 0 -roll +36+0 \) \
  \( -clone 0 -roll +48+0 \) \( -clone 0 -roll +60+0 \) \( -clone 0 -roll +72+0 \) \( -clone 0 -roll +84+0 \) \
  -layers optimize -colors 64 /tmp/axistv.gif
python3 - <<'PY'
data=open('/tmp/axistv.gif','rb').read()
open('kernel/sample_gif.h','w').write(
 "/* Auto-generated sample animated GIF for ClassicTube. See tools/gensample.sh. */\n"
 "#ifndef AXIS_SAMPLE_GIF_H\n#define AXIS_SAMPLE_GIF_H\n#include <stdint.h>\n"
 "static const uint8_t sample_gif[] = {\n"
 + "".join("  "+",".join(str(b) for b in data[i:i+20])+",\n" for i in range(0,len(data),20))
 + "};\n#define SAMPLE_GIF_LEN %d\n#endif\n"%len(data))
print("wrote kernel/sample_gif.h", len(data))
PY
