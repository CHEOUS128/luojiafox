"""Synthetic diagnostic only: static/moving edges, red/blue marks, darkness and cuts."""
from pathlib import Path
import argparse,subprocess,math
from PIL import Image,ImageDraw

def main():
    p=argparse.ArgumentParser();p.add_argument('--ffmpeg',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    if a.out.exists():raise RuntimeError('refusing to overwrite stress input')
    a.out.parent.mkdir(parents=True,exist_ok=True)
    cmd=[str(a.ffmpeg),'-hide_banner','-loglevel','error','-f','rawvideo','-pixel_format','rgb24','-video_size','640x512','-framerate','20','-i','pipe:0','-c:v','libx264rgb','-crf','0','-preset','ultrafast','-pix_fmt','rgb24','-n',str(a.out)]
    proc=subprocess.Popen(cmd,stdin=subprocess.PIPE,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    try:
        for i in range(240):
            t=i/20;move=0 if t<2 else int((t-2)*(22 if t<6 else 140));dark=1 if t<9 else .45
            im=Image.new('RGB',(640,512),tuple(int(v*dark) for v in (68,76,83)));d=ImageDraw.Draw(im)
            for x in range(-64,705,32):d.line((x-move%32,0,x-move%32,512),fill=(100,108,116),width=1)
            for y in range(0,512,32):d.line((0,y,640,y),fill=(90,98,106),width=1)
            # Fine texture, not noise synthesized from a future frame.
            for y in range(280,400,4):
                for x in range(250,390,4):
                    if ((x+y)//4)%2:d.rectangle((x,y,x+1,y+1),fill=(150,150,150))
            for j,col in enumerate([(240,32,32),(32,64,240)]):
                x=130+j*290+int(60*math.sin(move/80+j));y=150+int(40*math.sin(move/100+j))
                d.rectangle((x-45,y-30,x+45,y+30),fill=(28,30,32),outline=(180,180,180),width=2)
                for k in [-30,30]:d.rectangle((x+k-3,y-20,x+k+3,y+20),fill=col)
                d.text((x-5,y-5),'R' if j==0 else 'B',fill=(245,245,245))
            for box,col in [((0,0,18,18),(255,0,0)),((621,0,639,18),(0,255,0)),((0,493,18,511),(0,0,255)),((621,493,639,511),(255,255,0))]:d.rectangle(box,fill=col)
            if t>=10 and i%20<10:d.rectangle((200,80,440,130),fill=(235,235,235));d.text((220,100),'SCENE CHANGE',fill=(20,20,20))
            d.text((22,23),f'SYNTHETIC  frame={i}  {t:.2f}s',fill=(255,255,255))
            proc.stdin.write(im.tobytes())
    finally:proc.stdin.close()
    if proc.wait()!=0:raise RuntimeError('stress encoding failed')
    print('SYNTHETIC 12s / 20fps / 640x512:',a.out)
if __name__=='__main__':main()
