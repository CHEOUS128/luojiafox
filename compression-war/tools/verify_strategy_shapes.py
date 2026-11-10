"""Protocol/FOV checks only; headless runs never count as display-fps acceptance."""
from pathlib import Path
import argparse,json
from PIL import Image,ImageStat
from strategy_compare import rows,settings,fit,digest
from run_strategies import run
p=argparse.ArgumentParser();p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--fixtures',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
root=a.root.resolve();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False);result=[]
for profile in ['one-160','one-320','one-320-aq0','one-320-gop20','gray']:
    for kind,name in [('wide','四角-横屏.mp4'),('portrait','四角-竖屏.mp4'),('vfr','变帧间隔.mp4')]:
        src=(a.fixtures/name).resolve();dest=a.out/(profile+'-'+kind)
        run([root/'cw_live.exe','--input',src,'--profile',root/'profiles'/f'{profile}.ini','--headless','--offline-metrics','--out',dest],a.out/(dest.name+'.log'),root,120)
        run([root/'cw_wire_verify.exe','--fast',dest],a.out/(dest.name+'-replay.log'),root,120)
        s=settings(dest/'summary.txt');c=settings(dest/'strategy.ini');assert s['error']=='' and int(s['decoded_unassociated'])==0
        decoded=rows(dest/'processed.csv');assert decoded and all(int(f['width'])==int(c['width']) and int(f['height'])==int(c['height']) for f in decoded)
        corners=[]
        if kind!='vfr':
            source=rows(dest/'source-map.csv')[0];sw,sh=int(source['width']),int(source['height']);cw,ch=int(c['width']),int(c['height']);x,y,w,h=fit(sw,sh,cw,ch)
            im=Image.open(dest/'last-received.png').convert('RGB')
            for u,v in [(24,24),(sw-24,24),(24,sh-24),(sw-24,sh-24)]:
                xx=int(x+u*w/sw);yy=int(y+v*h/sh);corners.append(ImageStat.Stat(im.crop((xx-2,yy-2,xx+3,yy+3))).mean)
            if profile=='gray':assert all(max(rgb)-min(rgb)<8 and sum(rgb)/3>10 for rgb in corners),corners
            else:
                red,green,blue,yellow=corners
                assert red[0]>max(red[1:])+20 and green[1]>max(green[0],green[2])+20 and blue[2]>max(blue[:2])+20 and min(yellow[:2])>yellow[2]+20,corners
        result.append(dict(profile=profile,kind=kind,input_sha256=digest(src),source=int(s['source_frames']),decoded=int(s['decoded']),corners_rgb=corners,display_acceptance='NOT_TESTED_HEADLESS'))
        print('PASS FOV/protocol',profile,kind,flush=True)
(a.out/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
