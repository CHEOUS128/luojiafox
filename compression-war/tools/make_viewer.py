#!/usr/bin/env python3
"""
make_viewer.py —— 把仿真器的输出打成一个**自包含的**左右对比 HTML，
单文件、可发给别人、双击就能播。

为什么不直接在 cw_sim 里写死：
  cw_sim 的导出（viewer/left + viewer/right + player.html）依赖成百上千个小 PNG，
  换台电脑一拷就容易漏文件、路径也会断。这个脚本把它们压成 base64 内嵌，
  出来的 .html 一个文件就是全部。

用法：
  python make_viewer.py --run runs/v60 --out run_v60.html [--max-width 960]

输入：--run 指向 cw_sim 的 --out 目录（里面要有 viewer/left、viewer/right、player.html
      里嵌的 RIGHT 时间轴），或者直接指向 viewer 目录。
"""
import argparse
import base64
import json
import os
import re
import sys


def read_player_meta(player_html):
    """从 player.html 里把 LEFT_N / FPS / RIGHT / LOST 抠出来。"""
    txt = open(player_html, "r", encoding="utf-8").read()
    def num(name):
        m = re.search(r"const\s+%s\s*=\s*(-?[\d.]+)" % name, txt)
        if not m:
            raise SystemExit("player.html 里找不到 " + name)
        return float(m.group(1))
    m = re.search(r"const\s+RIGHT\s*=\s*(\[.*?\]);", txt, re.S)
    if not m:
        raise SystemExit("player.html 里找不到 RIGHT 时间轴")
    right = json.loads(m.group(1))
    m = re.search(r"const\s+LOST\s*=\s*new\s+Set\((\[.*?\])\)", txt, re.S)
    lost = json.loads(m.group(1)) if m else []
    m = re.search(r"const\s+W\s*=\s*(\d+)\s*,\s*H\s*=\s*(\d+)", txt)
    w, h = (int(m.group(1)), int(m.group(2))) if m else (192, 152)
    # 左图尺寸可以和右图不同：给了 --ref 就是 1280x1024，否则同 W/H
    m = re.search(r"const\s+LW\s*=\s*(\d+)\s*,\s*LH\s*=\s*(\d+)", txt)
    lw, lh = (int(m.group(1)), int(m.group(2))) if m else (w, h)
    return int(num("LEFT_N")), num("FPS"), w, h, lw, lh, right, lost


def b64_png(path):
    with open(path, "rb") as f:
        return "data:image/png;base64," + base64.b64encode(f.read()).decode("ascii")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True, help="cw_sim 的 --out 目录，或其中的 viewer 目录")
    ap.add_argument("--out", required=True, help="输出的单文件 html")
    ap.add_argument("--max-width", type=int, default=1100)
    args = ap.parse_args()

    base = args.run
    if os.path.isdir(os.path.join(base, "viewer")):
        base = os.path.join(base, "viewer")
    left_dir = os.path.join(base, "left")
    right_dir = os.path.join(base, "right")
    player = os.path.join(base, "player.html")
    for p in (left_dir, right_dir, player):
        if not os.path.exists(p):
            raise SystemExit("缺少 " + p)

    left_n, fps, w, h, lw, lh, right, lost = read_player_meta(player)

    left = []
    for i in range(left_n):
        f = os.path.join(left_dir, "L%05d.png" % i)
        if not os.path.exists(f):
            raise SystemExit("缺帧 " + f)
        left.append(b64_png(f))

    right_img = {}
    for k, item in enumerate(right):
        f = os.path.join(right_dir, "R%05d.png" % item["i"])
        if not os.path.exists(f):
            raise SystemExit("缺帧 " + f)
        right_img[k] = b64_png(f)

    def js_array(items):
        return "[" + ",".join('"%s"' % s for s in items) + "]"

    right_js = [{k: item[k] for k in ("enc", "t")} for item in right]
    right_js = [dict(RIGHT=right_js)]

    html = """<!doctype html>
<meta charset="utf-8">
<title>压片仿真 · 左右对比</title>
<style>
  :root{color-scheme:dark}
  *{box-sizing:border-box}
  body{margin:0;background:#0d0f12;color:#d7dce3;
       font:13px/1.5 "Segoe UI","Microsoft YaHei",system-ui,sans-serif}
  header{padding:16px 22px 10px;border-bottom:1px solid #1e242c}
  h1{margin:0;font-size:15px;font-weight:600;letter-spacing:.02em}
  .sub{margin-top:3px;color:#7f8896;font-size:12px}
  .wrap{display:flex;gap:14px;padding:14px 22px;align-items:flex-start;
        max-width:%(maxw)dpx;margin:0 auto}
  .pane{flex:1;min-width:0}
  .pane h2{margin:0 0 6px;font-size:11px;font-weight:600;letter-spacing:.08em;
           text-transform:uppercase;color:#6f7885;white-space:nowrap;overflow:hidden;
           text-overflow:ellipsis}
  canvas{display:block;width:100%%;height:auto;background:#000;
         border:1px solid #232a33;border-radius:3px}
  .tag{display:inline-block;margin-left:8px;padding:0 6px;border-radius:3px;
       font-size:10px;font-weight:600;letter-spacing:.04em;vertical-align:1px}
  .tag.frozen{background:#4a2b16;color:#ffb273}
  .bar{max-width:%(maxw)dpx;margin:0 auto;padding:12px 22px;border-top:1px solid #1e242c}
  input[type=range]{width:100%%;accent-color:#4b9fff}
  .row{display:flex;gap:14px;align-items:center;margin-top:9px;flex-wrap:wrap}
  button{background:#1a2029;color:#cdd4de;border:1px solid #2b333f;border-radius:4px;
         padding:5px 12px;font:inherit;font-size:12px;cursor:pointer}
  button:hover{background:#232c38;border-color:#3a4552}
  .stat{font-variant-numeric:tabular-nums;color:#6f7885}
  .stat b{color:#c8d1dc;font-weight:600}
  .legend{max-width:%(maxw)dpx;margin:0 auto;padding:4px 22px 24px;
          color:#5f6875;font-size:11.5px;line-height:1.75}
  .legend code{background:#161b22;padding:1px 5px;border-radius:3px;color:#8b98a8}
</style>
<header>
  <h1>压片仿真 · 左右对比</h1>
  <div class="sub">%(sub)s</div>
</header>
<div class="wrap">
  <div class="pane"><h2>左 · 真原片 %(lw)d×%(lh)d</h2><canvas id="L"></canvas></div>
  <div class="pane"><h2>右 · 客户端收到 %(w)d×%(h)d <span id="tag" class="tag"></span></h2><canvas id="R"></canvas></div>
</div>
<div class="bar">
  <input type="range" id="seek" min="0" max="1000" value="0">
  <div class="row">
    <button id="play">▶ 播放</button>
    <button id="slower">－ 慢</button>
    <button id="faster">＋ 快</button>
    <button id="nextlost">跳到下一个丢帧</button>
    <span class="stat">t = <b id="t">0.000</b> s</span>
    <span class="stat">左 <b id="lf">0</b></span>
    <span class="stat">右 <b id="rf">—</b></span>
    <span class="stat">丢帧 <b id="lost">0</b> / %(left_n)d</span>
  </div>
</div>
<div class="legend">
  左栏是<b>真原片</b>（同一取景、没经过编码），右栏是客户端真正收到的。两栏屏幕一样宽，
  所以右边的糊是真的糊。右侧画面按 <code>这一帧最后一个包发出的时刻</code> 切换，不是按名义帧率。
  所以丢帧不会“悄悄少一帧”，而是 <b>画面冻住、时间轴跳格</b>——和实车肉眼看到的一致。
  左图始终按名义帧率播放，作为时间基准。
</div>
<script>
const LEFT_N = %(left_n)d, FPS = %(fps)g, W = %(w)d, H = %(h)d;
const LW = %(lw)d, LH = %(lh)d;
const RIGHT = %(right)s;
const LOST = new Set(%(lost)s);
const SRC_L = %(left)s;
const SRC_R = %(right_img)s;
</script>
<script>
const cvL = document.getElementById('L'), cvR = document.getElementById('R');
cvL.width = LW; cvL.height = LH; cvR.width = W; cvR.height = H;
const cL = cvL.getContext('2d'), cR = cvR.getContext('2d');
// 两栏 CSS 撑到一样宽；左图本来够大，右图放大时浏览器平滑 = 实车"小图铺满屏"的观感
cL.imageSmoothingEnabled = true; cR.imageSmoothingEnabled = true;

const imgL = SRC_L.map(s => { const i = new Image(); i.src = s; return i; });
const imgR = SRC_R.map(s => { const i = new Image(); i.src = s; return i; });

// 给定时刻 → 最后一个到齐的解码帧下标（-1 = 还没收到任何帧）
function rightAt(tMs) {
  let lo = 0, hi = RIGHT.length - 1, ans = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (RIGHT[mid].t <= tMs) { ans = mid; lo = mid + 1; } else hi = mid - 1;
  }
  return ans;
}

const totalMs = (LEFT_N / FPS) * 1000;
let t = 0, playing = false, lastWall = 0, rate = 1.0, drawnL = -1, drawnR = -2;
const elT = document.getElementById('t'), elLF = document.getElementById('lf');
const elRF = document.getElementById('rf'), elLost = document.getElementById('lost');
const elTag = document.getElementById('tag'), seek = document.getElementById('seek');
const elPlay = document.getElementById('play');

function draw() {
  const li = Math.min(LEFT_N - 1, Math.floor(t / 1000 * FPS));
  if (li !== drawnL && imgL[li] && imgL[li].complete) { cL.drawImage(imgL[li], 0, 0); drawnL = li; }
  const ri = rightAt(t);
  if (ri !== drawnR) {
    if (ri >= 0 && imgR[ri] && imgR[ri].complete) { cR.drawImage(imgR[ri], 0, 0); drawnR = ri; }
  }
  elT.textContent = (t / 1000).toFixed(3);
  elLF.textContent = li;
  if (ri >= 0) {
    const enc = RIGHT[ri].enc, behind = Math.max(0, li - enc);
    elRF.textContent = '帧 ' + enc + '（落后 ' + behind + '）';
    const frozen = behind >= 3;
    elTag.className = 'tag' + (frozen ? ' frozen' : '');
    elTag.textContent = frozen ? '冻结' : '';
  } else {
    elRF.textContent = '—'; elTag.className = 'tag'; elTag.textContent = '';
  }
  elLost.textContent = LOST.size;
  seek.value = String(Math.round(t / totalMs * 1000));
}

function tick(ts) {
  if (!playing) return;
  if (lastWall) t += (ts - lastWall) * rate;
  lastWall = ts;
  if (t >= totalMs) { t = totalMs; playing = false; elPlay.textContent = '▶ 播放'; }
  draw();
  if (playing) requestAnimationFrame(tick);
}

elPlay.onclick = () => {
  playing = !playing;
  elPlay.textContent = playing ? '❚❚ 暂停' : '▶ 播放';
  if (playing) { lastWall = 0; if (t >= totalMs) t = 0; requestAnimationFrame(tick); }
};
document.getElementById('slower').onclick = () => rate = Math.max(0.1, rate / 1.5);
document.getElementById('faster').onclick = () => rate = Math.min(8, rate * 1.5);
seek.oninput = () => { t = totalMs * (seek.value / 1000); draw(); };
document.getElementById('nextlost').onclick = () => {
  const sorted = Array.from(LOST).sort((a, b) => a - b);
  for (const idx of sorted) {
    const when = idx / FPS * 1000;
    if (when > t + 1) { t = when; draw(); return; }
  }
  t = totalMs; draw();
};
draw();
</script>
""" % dict(
        maxw=args.max_width,
        sub="%d 帧 · %g fps · 左 %d×%d / 右 %d×%d · 解码成功 %d / %d" % (
            left_n, fps, lw, lh, w, h, len(right), left_n),
        left_n=left_n,
        fps=fps,
        w=w,
        h=h,
        lw=lw,
        lh=lh,
        right=json.dumps(right_js[0], separators=(",", ":")),
        lost=json.dumps(lost, separators=(",", ":")),
        left=js_array(left),
        right_img=js_array([right_img[k] for k in range(len(right))]),
    )

    with open(args.out, "w", encoding="utf-8") as f:
        f.write(html)
    mb = os.path.getsize(args.out) / (1024 * 1024)
    print("写好了 %s（%.1f MB，单文件）" % (args.out, mb))


if __name__ == "__main__":
    main()
