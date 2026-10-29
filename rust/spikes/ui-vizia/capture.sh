#!/usr/bin/env bash
# R0.3: captures the Vizia Track page from a real window under Xvfb (Vizia has no offscreen renderer).
#   ./capture.sh <output directory>     needs Xvfb, ImageMagick `import`, Mesa
set -euo pipefail
out=${1:?output directory}
mkdir -p "$out"
bin=$(dirname "$0")/target/release/spike-ui-vizia
for theme in light dark; do
  for scale in 1 1.5 2 2.5; do
    pct=$(awk "BEGIN{print int($scale*100)}")
    w=$(awk "BEGIN{print int(380*$scale)}"); h=$(awk "BEGIN{print int(640*$scale)}")
    xvfb-run -a -s "-screen 0 1600x1800x24" bash -c "
      HS_THEME=$theme WINIT_X11_SCALE_FACTOR=$scale LIBGL_ALWAYS_SOFTWARE=1 '$bin' & pid=\$!
      sleep 3
      import -window root -crop ${w}x${h}+0+0 '$out/vizia-$theme-$pct.png'
      kill \$pid"
    echo "$out/vizia-$theme-$pct.png"
  done
done
