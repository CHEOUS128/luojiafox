#include "viewer.h"

#include "platform_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

// ⚠ STB_IMAGE_WRITE_IMPLEMENTATION 只在 sim_runner.cpp 里定义一次，
//   这里只声明接口，否则会重复符号。
#include "stb_image_write.h"

namespace cw {

namespace {

// BT.601 有限范围 YUV → RGB。ffmpeg 解出来的 yuv420p 基本都是 TV range。
void yuv420p_to_rgb(const std::uint8_t * y_plane, int w, int h, std::vector<std::uint8_t> & rgb) {
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    const std::uint8_t * u_plane = y_plane + plane;
    const std::uint8_t * v_plane = u_plane + plane / 4;
    const int cw = w / 2;
    rgb.resize(plane * 3);
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            const int Y = y_plane[static_cast<std::size_t>(py) * w + px];
            const int U = u_plane[static_cast<std::size_t>(py / 2) * cw + px / 2];
            const int V = v_plane[static_cast<std::size_t>(py / 2) * cw + px / 2];
            const int c = 1164 * (Y - 16);
            const int d = U - 128, e = V - 128;
            const int r = (c + 1596 * e) / 1000;
            const int g = (c - 392 * d - 813 * e) / 1000;
            const int b = (c + 2017 * d) / 1000;
            auto clip = [](int v) -> std::uint8_t {
                return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
            };
            const std::size_t o = (static_cast<std::size_t>(py) * w + px) * 3;
            rgb[o + 0] = clip(r);
            rgb[o + 1] = clip(g);
            rgb[o + 2] = clip(b);
        }
    }
}

void gray_to_rgb(const std::uint8_t * y, std::size_t n, std::vector<std::uint8_t> & rgb) {
    rgb.resize(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t v = y[i];
        rgb[i * 3 + 0] = v;
        rgb[i * 3 + 1] = v;
        rgb[i * 3 + 2] = v;
    }
}

// 相机原片是 BGR（OpenCV 的顺序），PNG 要 RGB。
void bgr_to_rgb(const std::uint8_t * bgr, std::size_t n, std::vector<std::uint8_t> & rgb) {
    rgb.resize(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        rgb[i * 3 + 0] = bgr[i * 3 + 2];
        rgb[i * 3 + 1] = bgr[i * 3 + 1];
        rgb[i * 3 + 2] = bgr[i * 3 + 0];
    }
}

// 把一张 PNG 编进内存。
// ⚠ 不能用 stbi_write_png：它内部是 fopen，Windows 上按 ANSI(GBK) 解释路径，
//   输出目录里带中文就会写失败或写到错的地方。改用带回调的版本，
//   内存攒好之后交给 platform_util::write_file（CreateFileW）。
void png_callback(void * context, void * data, int size) {
    auto * buf = static_cast<std::vector<std::uint8_t> *>(context);
    const auto * p = static_cast<const std::uint8_t *>(data);
    buf->insert(buf->end(), p, p + size);
}

bool write_png(const std::string & path, const std::vector<std::uint8_t> & rgb, int w, int h) {
    std::vector<std::uint8_t> png;
    if (stbi_write_png_to_func(&png_callback, &png, w, h, 3, rgb.data(), w * 3) == 0) {
        return false;
    }
    std::string err;
    return write_file(path, png.data(), png.size(), err);
}

}  // namespace

bool export_viewer(const ViewerSpec & spec,
                   const std::vector<const std::uint8_t *> & src_planes,
                   const std::vector<ViewerFrame> & decoded,
                   std::string & error) {
    if (spec.out_dir.empty()) { error = "没给导出目录"; return false; }
    if (spec.width <= 0 || spec.height <= 0) { error = "宽高非法"; return false; }

    const std::size_t plane = static_cast<std::size_t>(spec.width) * spec.height;
    const std::size_t left_n = std::min(spec.frame_count, src_planes.size());
    if (left_n == 0) { error = "原始帧为空"; return false; }

    // ⚠ 这里以前是 system("mkdir -p ...")。Windows 上 system 走 cmd.exe，
    //   中文路径按 GBK 解释会错位；换成平台层递归建目录。
    if (!ensure_dir(spec.out_dir + "/left", error)) return false;
    if (!ensure_dir(spec.out_dir + "/right", error)) return false;

    // ---- 左：优先用「真原片」(ref)，没有才退回编码器输入 ----
    //
    // ★ CH 2026-11-07：左栏 = 相机原片，**原封不动**。相机录下来是彩色 BGR 就放彩色，
    //   不转灰度、不缩放、不锐化——那些都是"我们这边后续做的处理"，只允许出现在右栏。
    //
    // 为什么要有 ref：编码器输入只有 192×152，把它当左侧原图，人眼看到的是
    // 两栏一样糊，分不清"压缩损失"和"本来就这么大"。真原片是 1280×1024，
    // 浏览器里两栏显示尺寸相同 → 右边一糊立刻能认出来。
    //
    // 注意：ref 只用于画左图，不参与编码/封包/画质统计。链路仍然只吃 src。
    const bool use_ref = !spec.ref_planes.empty() && spec.ref_width > 0 && spec.ref_height > 0;
    const int ref_w = use_ref ? spec.ref_width : spec.width;
    const int ref_h = use_ref ? spec.ref_height : spec.height;
    const std::size_t ref_plane = static_cast<std::size_t>(ref_w) * ref_h;
    // 左栏到底是什么颜色：有原片看原片，没原片就退回编码器输入（可能是彩色 yuv420p）
    const bool left_color = use_ref ? spec.ref_is_bgr : !spec.gray;

    std::vector<std::uint8_t> rgb;
    for (std::size_t i = 0; i < left_n; ++i) {
        const std::uint8_t * p = nullptr;
        int pw = spec.width, ph = spec.height;
        if (use_ref) {
            if (i >= spec.ref_planes.size() || spec.ref_planes[i] == nullptr) continue;
            p = spec.ref_planes[i];
            pw = ref_w;
            ph = ref_h;
            if (spec.ref_is_bgr) bgr_to_rgb(p, ref_plane, rgb);
            else gray_to_rgb(p, ref_plane, rgb);
        } else {
            p = src_planes[i];
            if (p == nullptr) {
                // 编码线程丢掉的帧没有编码器输入可显示 → 沿用上一张。
                // 只在没给 --ref 时才会走到这里，纯粹是兜底。
                if (i > 0 && src_planes[i - 1] != nullptr) p = src_planes[i - 1];
                else continue;
            }
            if (spec.gray) gray_to_rgb(p, plane, rgb);
            else yuv420p_to_rgb(p, spec.width, spec.height, rgb);
        }
        char name[512];
        std::snprintf(name, sizeof(name), "%s/left/L%05zu.png", spec.out_dir.c_str(), i);
        if (!write_png(name, rgb, pw, ph)) {
            error = std::string("写 ") + name + " 失败";
            return false;
        }
    }

    // ---- 右：客户端真正解出来的帧，按解码顺序编号 ----
    std::vector<std::size_t> right_written(decoded.size(), 0);
    for (std::size_t k = 0; k < decoded.size(); ++k) {
        const auto & d = decoded[k];
        if (d.y_plane == nullptr || d.width != spec.width || d.height != spec.height) continue;
        gray_to_rgb(d.y_plane, plane, rgb);     // 解码器输出的是 Y 平面，等比铺成三通道
        char name[512];
        std::snprintf(name, sizeof(name), "%s/right/R%05zu.png", spec.out_dir.c_str(), k);
        if (!write_png(name, rgb, spec.width, spec.height)) {
            error = std::string("写 ") + name + " 失败";
            return false;
        }
        right_written[k] = 1;
    }

    // ---- player.html ----
    // 先用流拼完整段 HTML 再一次性落盘：路径同样要走宽字符接口。
    const std::string html_path = spec.out_dir + "/player.html";
    std::ostringstream f;

    // 右侧时间轴：每一帧解出来的画面 + 它"处理完、能看见"的时刻
    std::ostringstream right_json;
    right_json << '[';
    bool first = true;
    double right_end_ms = 0.0;
    for (std::size_t k = 0; k < decoded.size(); ++k) {
        if (!right_written[k]) continue;
        if (!first) right_json << ',';
        first = false;
        right_json << "{\"i\":" << k << ",\"enc\":" << decoded[k].encoded_index
                   << ",\"t\":" << decoded[k].ready_ms
                   << ",\"td\":" << decoded[k].encode_done_ms << '}';
        right_end_ms = std::max(right_end_ms, decoded[k].ready_ms);
    }
    right_json << ']';

    // 哪些编码帧客户端从没看见过（右侧会一直冻在上一张）
    std::vector<char> seen(left_n, 0);
    for (const auto & d : decoded) if (d.encoded_index < left_n) seen[d.encoded_index] = 1;
    std::ostringstream lost_json;
    lost_json << '[';
    bool first_lost = true;
    for (std::size_t i = 0; i < left_n; ++i) {
        if (seen[i]) continue;
        if (!first_lost) lost_json << ',';
        first_lost = false;
        lost_json << i;
    }
    lost_json << ']';

    // 时间轴长度：至少要覆盖"左侧放完 + 右侧最后一帧出现"，否则最后几帧的延迟看不到
    const double left_end_ms = static_cast<double>(left_n) / std::max(1e-6, spec.fps) * 1000.0;
    const double total_ms = std::max(left_end_ms, right_end_ms) + 250.0;

    const std::string title = spec.title;
    const std::string subtitle = spec.subtitle;
    // ⚠ 数字别直接嵌在 R"( ... )" 里：`R"(, H = )"` 会被当成分隔符提前结束 R"(
    //   这里先拼成一段普通的 JS，再整段插进模板，避免"逗号+空格+w"那个坑。
    std::ostringstream js_head;
    js_head << "const LEFT_N = " << left_n << ";\n"
            << "const FPS = " << spec.fps << ";\n"
            << "const W = " << spec.width << ", H = " << spec.height << ";\n"
            // 左图尺寸可以和右图不同：给了 ref 就是原分辨率，没给就同 W/H。
            // 两栏在 CSS 里都撑满半屏，所以"左边清楚、右边糊"是分辨率差带来的真差异。
            << "const LW = " << ref_w << ", LH = " << ref_h << ";\n"
            << "const RIGHT = " << right_json.str() << ";\n"
            << "const LOST = new Set(" << lost_json.str() << ");\n"
            << "const TOTAL_MS = " << total_ms << ";\n"
            << "const ENC_MS = " << spec.encode_ms << ";\n"
            << "const WIRE_MS = " << spec.wire_extra_ms << ";\n"
            << "const ENC_DROP = " << spec.encode_dropped << ";\n";

    f << R"(<!doctype html>
<meta charset="utf-8">
<title>)" << title << R"(</title>
<style>
  :root{color-scheme:dark}
  *{box-sizing:border-box}
  body{margin:0;background:#0d0f12;color:#d7dce3;
       font:13px/1.5 "Segoe UI","Microsoft YaHei",system-ui,sans-serif}
  header{padding:14px 20px 8px;border-bottom:1px solid #1e242c}
  h1{margin:0;font-size:15px;font-weight:600;letter-spacing:.02em}
  .sub{margin-top:2px;color:#7f8896;font-size:12px}
  .wrap{display:flex;gap:14px;padding:14px 20px;align-items:flex-start}
  .pane{flex:1;min-width:0}
  .pane h2{margin:0 0 6px;font-size:11px;font-weight:600;letter-spacing:.08em;
           text-transform:uppercase;color:#6f7885}
  .pane h2 b{color:#9aa6b6;font-weight:600}
  canvas{display:block;width:100%;height:auto;background:#000;
         border:1px solid #232a33;border-radius:3px}
  .tag{display:inline-block;margin-left:8px;padding:0 6px;border-radius:3px;
       font-size:10px;font-weight:600;letter-spacing:.04em}
  .tag.frozen{background:#4a2b16;color:#ffb273}
  .tag.nodelay{background:#3a2416;color:#ffcf9e}
  .bar{margin:0 20px;padding:12px 0;border-top:1px solid #1e242c}
  input[type=range]{width:100%;accent-color:#4b9fff}
  .row{display:flex;gap:16px;align-items:center;margin-top:8px;flex-wrap:wrap}
  button{background:#1a2029;color:#cdd4de;border:1px solid #2b333f;border-radius:4px;
         padding:5px 12px;font:inherit;font-size:12px;cursor:pointer}
  button:hover{background:#232c38;border-color:#3a4552}
  .stat{font-variant-numeric:tabular-nums;color:#6f7885}
  .stat b{color:#c8d1dc;font-weight:600}
  .stat.hi b{color:#7fc4ff}
  .legend{padding:0 20px 20px;color:#5f6875;font-size:11.5px;line-height:1.7}
  .legend code{background:#161b22;padding:1px 5px;border-radius:3px;color:#8b98a8}
</style>
<header>
  <h1>)" << title << R"(</h1>
  <div class="sub">)" << subtitle << R"(</div>
</header>
<div class="wrap">
  <div class="pane"><h2>左 · 相机原片 <b>)" << ref_w << "×" << ref_h
    << (left_color ? " 彩色" : " 灰度") << R"(</b>（未处理）</h2>
    <canvas id="L"></canvas></div>
  <div class="pane"><h2>右 · 处理完、客户端收到 <b>)" << spec.width << "×" << spec.height
    << (spec.gray ? " 灰度" : " 彩色") << R"(</b> <span id="tag"></span></h2>
    <canvas id="R"></canvas></div>
</div>
<div class="bar">
  <input type="range" id="seek" min="0" max="1000" value="0">
  <div class="row">
    <button id="play">▶ 播放</button>
    <button id="slower">－ 慢</button>
    <button id="faster">＋ 快</button>
    <button id="realtime">实时 1×</button>
    <button id="nextlost">跳到下一个丢帧</button>
    <span class="stat">t = <b id="t">0.000</b> s</span>
    <span class="stat">左 帧 <b id="lf">0</b>（原始）</span>
    <span class="stat">右 帧 <b id="rf">—</b>（处理完）</span>
    <span class="stat hi">延迟 <b id="lag">—</b> ms</span>
    <span class="stat">落后 <b id="behind">0</b> 帧</span>
    <span class="stat">丢帧 <b id="lost">0</b> / )" << left_n << R"(</span>
  </div>
</div>
<div class="legend">
  左栏是<b>相机原片，原封不动</b>（)" << ref_w << R"(×)" << ref_h << R"(，" << (left_color ? "彩色" : "灰度") << R"(）——
  转灰度、缩放、编码都是我们这边后续做的处理，只出现在右栏。
  <b>同一帧左边先出现，右边要等它处理完、包发完才出现</b>，两者的时间差就是下面这个「延迟」。
  右侧按 <code>处理完的时刻</code> 切换；丢帧不会"悄悄少一帧"，而是<b>画面冻住、落后帧数往上跳</b>。
  <span id="encnote"></span>
</div>
<script>
)" << js_head.str() << R"(
const cvL = document.getElementById('L'), cvR = document.getElementById('R');
// 两块 canvas 各自用自己那张图的分辨率，CSS 再把它们拉到一样宽。
// 这样左栏是真原片细节，右栏只有 W×H（放大后自然糊）——对比才有意义。
cvL.width = LW; cvL.height = LH; cvR.width = W; cvR.height = H;
const cL = cvL.getContext('2d'), cR = cvR.getContext('2d');
cL.imageSmoothingEnabled = true; cR.imageSmoothingEnabled = true;

const imgL = new Array(LEFT_N), imgR = new Array(RIGHT.length);
for (let i = 0; i < LEFT_N; i++) {
  const im = new Image();
  im.src = 'left/L' + String(i).padStart(5, '0') + '.png';
  imgL[i] = im;
}
for (let k = 0; k < RIGHT.length; k++) {
  const im = new Image();
  im.src = 'right/R' + String(RIGHT[k].i).padStart(5, '0') + '.png';
  imgR[k] = im;
}

// 左侧：t 时刻相机拍到的是第几帧（相机按名义帧率出图，左侧是时间基准）
function leftAt(tMs) {
  return Math.max(0, Math.min(LEFT_N - 1, Math.floor(tMs / 1000 * FPS)));
}
// 右侧：t 时刻**已经处理完**的最后一帧是第几个（-1 = 还没收到任何帧）
//
// ⚠ 判据是 `t >= max(整帧到齐, 编码器吐出这一帧)`，不能只看"最后一包什么时候发出去"。
//   编码线程串行、每帧占 encode_ms；相机在它手里的这段时间里又放行了新帧，于是
//   一帧的「最后一包发出」时刻可能**早于它在编码器里真正被编码完成**的时刻。
//   只看到齐时刻，播放器会在那一帧还躺在编码器里的时候就把它放出来——比实车快，
//   而且快出来的正是编码这一段，恰恰是这一栏最该显示的东西。
function rightAt(tMs) {
  let lo = 0, hi = RIGHT.length - 1, ans = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    const show = Math.max(RIGHT[mid].t, RIGHT[mid].td);
    if (show <= tMs) { ans = mid; lo = mid + 1; } else hi = mid - 1;
  }
  return ans;
}

let t = 0, playing = false, lastWall = 0, rate = 1.0;
let drawnL = -1, drawnR = -2;

const elT = document.getElementById('t'), elLF = document.getElementById('lf');
const elRF = document.getElementById('rf'), elLost = document.getElementById('lost');
const elTag = document.getElementById('tag'), seek = document.getElementById('seek');
const elLag = document.getElementById('lag'), elBehind = document.getElementById('behind');

// 把"这一帧处理花了多久"写在图例里：为 0 时必须明确说出来，别让人以为延迟只有这点
(function () {
  // ready_ms 里已经包含编码时间（包是编完才存在的），所以这里只列环节，不再做加法。
  let s = '　延迟口径：相机拍到'
        + ' → 编码（' + (ENC_MS > 0 ? ENC_MS.toFixed(1) + ' ms/帧' : '未计') + '）'
        + ' → 拆包排队、令牌桶按 40 包/秒整帧放行 → 最后一包到齐'
        + (WIRE_MS > 0 ? ' → 固定呈现 ' + WIRE_MS.toFixed(1) + ' ms' : '')
        + '。';
  if (ENC_DROP > 0) {
    s += '　⚠ 编码线程跟不上相机节拍，有 ' + ENC_DROP
       + ' 帧还没被取走就被下一帧覆盖（从没进过编码器），右栏在这些时刻会停在上一张。';
  }
  document.getElementById('encnote').textContent = s;
})();

function draw() {
  const li = leftAt(t);
  if (li !== drawnL && imgL[li] && imgL[li].complete) {
    cL.drawImage(imgL[li], 0, 0); drawnL = li;
  }
  const ri = rightAt(t);
  if (ri !== drawnR) {
    if (ri >= 0 && imgR[ri] && imgR[ri].complete) {
      cR.drawImage(imgR[ri], 0, 0);
      drawnR = ri;
    }
  }
  elT.textContent = (t / 1000).toFixed(3);
  elLF.textContent = li;
  if (ri >= 0) {
    const enc = RIGHT[ri].enc;
    // 这一帧从"被拍到"到"处理完能看见"一共多久
    const born = enc / FPS * 1000;
    elLag.textContent = Math.max(0, RIGHT[ri].t - born).toFixed(0);
    elRF.textContent = '帧 ' + enc;
    elBehind.textContent = Math.max(0, li - enc);
    // 右边已经一个多帧间隔没更新 → 冻住了
    const frozen = (li - enc) >= 3;
    elTag.className = 'tag' + (frozen ? ' frozen' : '');
    elTag.textContent = frozen ? '冻结' : '';
  } else {
    elRF.textContent = '—'; elLag.textContent = '—'; elBehind.textContent = '0';
    elTag.className = 'tag'; elTag.textContent = '';
  }
  elLost.textContent = LOST.size;
  seek.value = String(Math.round(t / TOTAL_MS * 1000));
}

function tick(ts) {
  if (!playing) return;
  if (lastWall) t += (ts - lastWall) * rate;
  lastWall = ts;
  if (t >= TOTAL_MS) { t = TOTAL_MS; playing = false;
    document.getElementById('play').textContent = '▶ 播放'; }
  draw();
  if (playing) requestAnimationFrame(tick);
}

document.getElementById('play').onclick = e => {
  playing = !playing;
  e.target.textContent = playing ? '❚❚ 暂停' : '▶ 播放';
  if (playing) { lastWall = 0; if (t >= TOTAL_MS) t = 0; requestAnimationFrame(tick); }
};
document.getElementById('slower').onclick = () => rate = Math.max(0.1, rate / 1.5);
document.getElementById('faster').onclick = () => rate = Math.min(8, rate * 1.5);
document.getElementById('realtime').onclick = () => rate = 1.0;
seek.oninput = () => { t = TOTAL_MS * (seek.value / 1000); draw(); };
document.getElementById('nextlost').onclick = () => {
  // 跳到下一个"左边已经有帧、右边还冻在更早的帧"的时刻
  const sorted = Array.from(LOST).sort((a, b) => a - b);
  for (const idx of sorted) {
    const when = idx / FPS * 1000;
    if (when > t + 1) { t = when; draw(); return; }
  }
  t = TOTAL_MS; draw();
};
draw();
</script>
)";

    {
        std::string err;
        if (!write_file(html_path, f.str().data(), f.str().size(), err)) {
            error = "写 " + html_path + " 失败：" + err;
            return false;
        }
    }
    return true;
}

}  // namespace cw
