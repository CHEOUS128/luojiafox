"""Make a simple landing page for existing, independently verified recordings."""
from pathlib import Path
import argparse,sys
if hasattr(sys.stdout,'reconfigure'):sys.stdout.reconfigure(encoding='utf-8')
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path(__file__).resolve().parents[1]/'runs/strategies');a=p.parse_args()
required=['visible-camera/index.html','先看这个-真实相机对比.mp4']
for name in required:
    if not (a.out/name).is_file():raise RuntimeError('Missing completed comparison: '+name)
html='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>压片战争：先看效果</title>
<style>body{margin:32px auto;padding:0 22px;max-width:1120px;font:18px/1.8 "Microsoft YaHei",sans-serif;background:#101720;color:#e5edf5}h1{font-size:32px}h2{font-size:23px}a{color:#83ddc7}.box{background:#1b2b3b;padding:18px 24px;margin:20px 0;border-radius:12px}video{width:100%;background:#060a0f;border-radius:8px}table{width:100%;border-collapse:collapse}td,th{padding:12px;text-align:left;border-bottom:1px solid #43566a}.button{display:inline-block;background:#087e75;color:white;padding:10px 18px;border-radius:8px;text-decoration:none;margin:5px}small{color:#a6bbce}summary{cursor:pointer}</style>
<h1>压片战争：先看看哪张图更好认</h1>
<p>电脑上的仿真已经跑通，也做出了几种可以对照的办法。画面目前还比较糊，这轮主要看线条、轮廓和移动中的物体有没有更容易认。</p>
<div class="box"><b>B 方案已在实车出画。</b><p>2026 年 11 月 12 日，队内通过官方图传在自定义客户端看到了工业相机画面。下面用同一段工业相机实录比较原方案、A 和 B。</p></div>
<table><tr><th>画面名字</th><th>它是什么</th><th>你主要看什么</th></tr>
<tr><td>原画面</td><td>传输前的画面，作参照</td><td>本来有哪些细节</td></tr>
<tr><td>原来的办法</td><td>之前最简单、已经跑通的方案</td><td>新方案到底有没有改善</td></tr>
<tr><td>A：兼顾清楚和恢复</td><td>对照方案，恢复相对较快</td><td>看线条、轮廓是否更容易认</td></tr>
<tr><td>B：更偏重细节</td><td>当前实车使用的方案</td><td>看车轮边缘和地面交叉线</td></tr></table>
<div class="box"><b>“慢”到底慢在哪？</b><p>正常播放时，A和B通常都只比原画面晚约0.1秒。故意漏掉一包数据的那次测试中，A约0.36秒恢复，B约0.86秒恢复。不是每张图都要等这么久，而是传输出了问题以后，B那次等得更久。</p></div>
<h2>先看真实相机拍的这段</h2><p>点三角形播放。四格从左到右、从上到下依次是：原画面、原来的办法、A、B。重点盯地上的交叉线、轮子和数字“1”。</p>
<video controls preload="metadata" src="先看这个-真实相机对比.mp4"></video>
<p><a class="button" href="visible-camera/index.html">打开大画面，可以暂停、放大</a></p>
<p><a href="../../docs/验收/策略探索-2026-11-09.md">查看各档实测数据</a></p></html>'''
(a.out/'index.html').write_text(html,encoding='utf-8',newline='\n')
(a.out/'latest-comparison.txt').write_text(str((a.out/'index.html').resolve()),encoding='utf-8')
print('Simple comparison ready:',a.out/'index.html')
