"""Optional offline preview. Actual packet/PNG proof remains the lossless authority."""
from pathlib import Path
import argparse,json,bisect,subprocess,sys
if hasattr(sys.stdout,'reconfigure'):sys.stdout.reconfigure(encoding='utf-8')
from PIL import Image,ImageDraw,ImageFont
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--ffmpeg',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
data=json.loads((a.root/'comparison-data.js').read_text(encoding='utf-8')[len('window.COMPARISON='):-1]);runs={r['name']:r for r in data['runs']}
names=['baseline','one-160','one-320-gop20'];selected=[None]+[runs[n] for n in names];samples=runs['baseline']['source'];end=max(s['sourceMs'] for s in samples)
fontfile=Path('C:/Windows/Fonts/msyh.ttc');font=ImageFont.truetype(str(fontfile),16) if fontfile.exists() else ImageFont.load_default()
cache={}
def frame(run,t):
    entries=samples if run is None else run['shown'];key='sourceMs' if run is None else 'presentMs'
    i=bisect.bisect_right([s[key] for s in entries],t)-1
    return entries[i] if i>=0 else None
def image(path):
    if path not in cache:
        cache[path]=Image.open(a.root/path).convert('RGB')
        if len(cache)>80:cache.pop(next(iter(cache)))
    return cache[path]
def panel(run,t):
    im=Image.new('RGB',(640,576),(18,26,36));d=ImageDraw.Draw(im);f=frame(run,t)
    label='原片（独立展示）' if run is None else run['label']
    d.text((12,8),label,font=font,fill=(230,240,250))
    if f:
        path=f"original/{f['fileIndex']}.png" if run is None else f['image'];pic=image(path)
        scale=min(640/pic.width,512/pic.height)
        pic=pic.resize((round(pic.width*scale),round(pic.height*scale)),Image.Resampling.BICUBIC)
        im.paste(pic,((640-pic.width)//2,36+(512-pic.height)//2))
        info=f"t={t/1000:.2f}s 源帧{f['index']}"
        if run is not None:info+=f" 画面年龄{t-f['sourceMs']:.0f}ms {f['codedBytes']}B/{f['packets']}包"
        d.text((12,550),info,font=font,fill=(130,220,205))
    else:d.text((160,280),'尚未收到解码图像',font=font,fill=(180,195,210))
    return im
a.out.parent.mkdir(parents=True,exist_ok=True)
cmd=[str(a.ffmpeg),'-hide_banner','-loglevel','error','-f','rawvideo','-pix_fmt','rgb24','-s','1280x1152','-r','20','-i','pipe:0','-an','-c:v','libx264','-preset','fast','-crf','18','-pix_fmt','yuv420p','-movflags','+faststart','-n',str(a.out)]
proc=subprocess.Popen(cmd,stdin=subprocess.PIPE)
try:
    for index in range(int(end/50)+2):
        t=index*50;im=Image.new('RGB',(1280,1152))
        for k,run in enumerate(selected):im.paste(panel(run,t),(k%2*640,k//2*576))
        proc.stdin.write(im.tobytes())
finally:proc.stdin.close()
if proc.wait():raise RuntimeError('preview encoding failed')
print('OFFLINE preview re-encoded for sharing; PNG replay remains pixel evidence:',a.out)
