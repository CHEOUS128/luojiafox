"""Offline, source-referenced comparison. Never runs in the transport/receiver process."""
from pathlib import Path
import argparse,csv,json,math,subprocess,hashlib,shutil,sys,re,collections
import numpy as np
from PIL import Image
if hasattr(sys.stdout,'reconfigure'):sys.stdout.reconfigure(encoding='utf-8')

def rows(path):
    with Path(path).open(encoding='utf-8-sig',newline='') as f:return list(csv.DictReader(f))
def settings(path):
    return dict(s.split('=',1) for s in Path(path).read_text(encoding='utf-8-sig').splitlines() if s and not s.startswith('#') and '=' in s)
def number(x):
    try:return float(x)
    except (ValueError,TypeError):return 0.
def digest(p):
    h=hashlib.sha256()
    with Path(p).open('rb') as f:
        while b:=f.read(1024*1024):h.update(b)
    return h.hexdigest()
def fit(sw,sh,tw,th):
    scale=min(tw/sw,th/sh);w=max(1,math.floor(sw*scale+.00001));h=max(1,math.floor(sh*scale+.00001))
    return (tw-w)//2,(th-h)//2,w,h
def metrics(ref,decoded):
    a=np.asarray(ref,dtype=np.float32);b=np.asarray(decoded,dtype=np.float32)
    ya=a[:,:,0]*.299+a[:,:,1]*.587+a[:,:,2]*.114;yb=b[:,:,0]*.299+b[:,:,1]*.587+b[:,:,2]*.114
    mse=float(np.mean((ya-yb)**2));psnr=10*math.log10(255**2/max(mse,1e-12))
    h,w=ya.shape;h=h//8*8;w=w//8*8
    def blocks(x):return x[:h,:w].reshape(h//8,8,w//8,8).transpose(0,2,1,3).reshape(-1,64)
    x,y=blocks(ya),blocks(yb);mx=x.mean(1);my=y.mean(1);vx=x.var(1);vy=y.var(1);cov=((x-mx[:,None])*(y-my[:,None])).mean(1)
    ss=((2*mx*my+6.5025)*(2*cov+58.5225))/((mx*mx+my*my+6.5025)*(vx+vy+58.5225))
    ca=a-np.stack([ya]*3,-1);cb=b-np.stack([yb]*3,-1)
    edge=(np.abs(np.diff(ya,axis=0)-np.diff(yb,axis=0)).mean()+np.abs(np.diff(ya,axis=1)-np.diff(yb,axis=1)).mean())/2
    def redblue(rgb):
        red=(rgb[:,:,0]>80)&(rgb[:,:,0]>rgb[:,:,2]*1.4)&(rgb[:,:,0]>rgb[:,:,1]*1.25)
        blue=(rgb[:,:,2]>80)&(rgb[:,:,2]>rgb[:,:,0]*1.4)&(rgb[:,:,2]>rgb[:,:,1]*1.25)
        return red.astype(np.int8)-blue.astype(np.int8)
    original_color=redblue(a);received_color=redblue(b);mask=original_color!=0;n=int(mask.sum())
    return dict(psnr_y=psnr,ssim_y_8x8=float(ss.mean()),color_mae=float(np.abs(ca-cb).mean()),edge_mae=float(edge),
        redblue_wrong_pixels=int(np.sum(received_color[mask]==-original_color[mask])),redblue_missing_pixels=int(np.sum(received_color[mask]==0)),redblue_reference_pixels=n)

class Source:
    def __init__(self,path,ffmpeg,cache):
        self.path=Path(path);self.ffmpeg=ffmpeg;self.cache=cache;cache.mkdir(parents=True,exist_ok=True)
        self.raw=self.path.suffix.lower() in ['.bgr','.raw','.bin'];self.file=self.path.open('rb') if self.raw else None
        self.prepared=False
    def get(self,index,w,h):
        dst=self.cache/f'{index}.png'
        if dst.exists():return Image.open(dst).convert('RGB')
        if self.raw:
            self.file.seek(index*w*h*3);data=self.file.read(w*h*3)
            if len(data)!=w*h*3:raise RuntimeError('source index outside raw video')
            im=Image.frombytes('RGB',(w,h),data,'raw','BGR')
            _,_,nw,nh=fit(w,h,640,512);im=im.resize((nw,nh),Image.Resampling.LANCZOS);im.save(dst)
        else:
            # Export only for offline inspection; never used by runtime receiver.
            if not self.prepared:
                cmd=[str(self.ffmpeg),'-hide_banner','-loglevel','error','-i',str(self.path),'-map','0:v:0','-vf',"scale=w='min(640,iw)':h='min(512,ih)':force_original_aspect_ratio=decrease",'-fps_mode','passthrough','-start_number','0',str(self.cache/'%d.png')]
                subprocess.run(cmd,check=True,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0));self.prepared=True
            if not dst.exists():raise RuntimeError(f'source frame export missing: {index}')
            im=Image.open(dst).convert('RGB')
        return im

def prepare_run(run,source,base):
    cfg=settings(run/'strategy.ini');summary=settings(run/'summary.txt')
    src={int(r['source_id']):r for r in rows(run/'source-map.csv')}
    frame={int(r['frame_id']):r for r in rows(run/'per_frame.csv')}
    assoc=rows(run/'decoded-association.csv');displays=rows(run/'display.csv')
    expected_pixels={int(r['ordinal']):r for r in rows(run/'processed.csv')}
    origin=min((float(s['source_ms']) for s in src.values()),default=0)
    decoded=[];qualities=[]
    for r in assoc:
        sid=int(r['source_id']);ordinal=int(r['ordinal']);sp=src.get(sid)
        if sp is None:continue
        img=run/'received-images'/f'{ordinal}.png'
        if not img.exists():raise RuntimeError(f'missing independent reconstruction: {img}')
        sw,sh=int(sp['width']),int(sp['height']);ref=source.get(int(sp['file_index']),sw,sh)
        rgba=Image.open(img).convert('RGBA');fp=expected_pixels[ordinal]
        if rgba.size!=(int(fp['width']),int(fp['height'])) or hashlib.sha256(rgba.tobytes()).hexdigest()!=fp['sha256']:
            raise RuntimeError(f'exported image differs from verified receiver output: {img}')
        rec=rgba.convert('RGB');cw,ch=int(cfg['width']),int(cfg['height']);x,y,w,h=fit(sw,sh,cw,ch)
        # Strip only the known letterbox; no scene crop or per-strategy reference filtering.
        scale=int(cfg['upscale']);rec=rec.crop((x*scale,y*scale,(x+w)*scale,(y+h)*scale)).resize(ref.size,Image.Resampling.BICUBIC)
        q=metrics(ref,rec);q.update(source_id=sid,ordinal=ordinal,source_index=int(sp['source_index']));qualities.append(q)
        fr=frame.get(sid,{})
        decoded.append(dict(sourceId=sid,index=int(sp['source_index']),fileIndex=int(sp['file_index']),ordinal=ordinal,sourceMs=float(sp['source_ms'])-origin,
            image=(Path(run.name)/'received-images'/img.name).as_posix(),codedBytes=int(fr.get('coded_bytes',0)),packets=int(fr.get('packets',0)),quality=q))
    qfile=run/'source-quality.csv'
    if qualities:
        with qfile.open('w',encoding='utf-8',newline='') as f:
            writer=csv.DictWriter(f,fieldnames=list(qualities[0]));writer.writeheader();writer.writerows(qualities)
    byord={r['ordinal']:r for r in decoded};shown=[]
    for d in displays:
        n=int(d['ordinal'])
        if n in byord:shown.append({**byord[n], 'presentMs':float(d['present_ms'])-origin,'latencyMs':float(d['present_ms'])-float(d['source_ms'])})
    def stats(vals):
        return dict(p50=float(np.median(vals)),p95=float(np.quantile(vals,.95)),max=float(np.max(vals))) if vals else dict(p50=None,p95=None,max=None)
    quality={k:float(np.mean([q[k] for q in qualities])) for k in ['psnr_y','ssim_y_8x8','color_mae','edge_mae']} if qualities else {}
    color_pixels=sum(q['redblue_reference_pixels'] for q in qualities)
    quality.update(redblue_reference_pixels=color_pixels,redblue_wrong=sum(q['redblue_wrong_pixels'] for q in qualities)/color_pixels if color_pixels else None,
                   redblue_missing=sum(q['redblue_missing_pixels'] for q in qualities)/color_pixels if color_pixels else None)
    gaps=np.diff([s['presentMs'] for s in shown]).tolist();ages=[]
    for i,s in enumerate(shown[:-1]):ages.append(shown[i+1]['presentMs']-s['sourceMs'])
    stages={}
    for name,a,b in [('pre','pre_start_ms','pre_end_ms'),('encode','encode_start_ms','encode_end_ms'),('decode','decode_start_ms','decode_end_ms'),('post','decode_end_ms','post_end_ms')]:
        stages[name]=stats([float(f[b])-float(f[a]) for f in frame.values() if float(f[a])>=0 and float(f[b])>=0])
    samples=[dict(index=int(s['source_index']),fileIndex=int(s['file_index']),sourceMs=float(s['source_ms'])-origin) for s in src.values()]
    payloads=collections.defaultdict(bytearray);txdata=(run/'tx-0310.bin').read_bytes();packet_rows=rows(run/'tx-times.csv')
    for i,entry in enumerate(packet_rows):
        packet=txdata[i*309+7:i*309+307];size=int.from_bytes(packet[:2],'big')-2;payloads[entry['source_frame']]+=packet[4:4+size]
    slice_hist=collections.Counter()
    for au in payloads.values():slice_hist[sum(1<=(au[m.end()]&31)<=5 for m in re.finditer(b'\x00\x00(?:\x00)?\x01',au) if m.end()<len(au))]+=1
    result=dict(name=cfg['name'],label=cfg['label'],family=cfg['family'],config=cfg,summary=summary,quality=quality,decoded=decoded,shown=shown,source=samples,actualVclNalsPerFrame=dict(slice_hist),packetTimes=[float(r['time_ms'])-origin for r in packet_rows],
        gaps=stats(gaps),agePeaks=stats(ages),stages=stages,freezeEvents200ms=sum(g>=200 for g in gaps),qualityCoverage=f'{len(qualities)}/{len(assoc)}',
        verification=settings(run/'wire-verification.txt'),postVerification=settings(run/'rx-replay-verification.txt'))
    (run/'comparison.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(f"ANALYZED {cfg['name']} decoded={len(decoded)} shown={len(shown)} SSIM={quality.get('ssim_y_8x8',0):.4f} colorMAE={quality.get('color_mae',0):.3f}",flush=True)
    return result

def build_comparison(root,input_path,ffmpeg):
    root=Path(root);source_hash=digest(input_path)
    manifest=json.loads((root/'experiment.json').read_text(encoding='utf-8'))
    if manifest['input_sha256']!=source_hash:raise RuntimeError('comparison source does not match recorded experiment input SHA256')
    source=Source(input_path,ffmpeg,root/'original');runs=[]
    for r in sorted(root.iterdir()):
        if (r/'strategy.ini').exists() and (r/'wire-verification.txt').exists():
            if not (r/'wire-verification.txt').read_text(encoding='utf-8').startswith('PASS'):continue
            runs.append(prepare_run(r,source,root))
            for row in rows(r/'source-map.csv'):source.get(int(row['file_index']),int(row['width']),int(row['height']))
    # Raw original frames are shared once across all candidate runs.
    dataset=dict(source=str(input_path),sourceSha256=source_hash,runs=runs,metric='原片统一最长640×512有效区域；PSNR-Y / 8×8块SSIM-Y / 色差MAE / 边缘误差；红蓝统计仅为饱和像素检查，不是队伍识别准确率；所有质量统计在运行后完成')
    (root/'comparison-data.js').write_text('window.COMPARISON='+json.dumps(dataset,ensure_ascii=False)+';',encoding='utf-8')
    shutil.copyfile(Path(__file__).with_name('strategy_viewer.html'),root/'index.html')
    columns=['name','label','ssim_y_8x8','color_mae','edge_mae','psnr_y','minimum_stable_display_fps','latency_median_ms','latency_p95_ms','oversize','max_sent_frame_h264_bytes','packets_sent','display_acceptance']
    with (root/'comparison.csv').open('w',encoding='utf-8-sig',newline='') as f:
        w=csv.DictWriter(f,fieldnames=columns);w.writeheader()
        for r in runs:w.writerow({k:({'name':r['name'],'label':r['label'],**r['quality'],**r['summary']}).get(k,'') for k in columns})
    print('COMPARISON',root/'index.html',flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--input',type=Path,required=True);p.add_argument('--ffmpeg',type=Path,required=True);a=p.parse_args()
    build_comparison(a.root,a.input,a.ffmpeg)
