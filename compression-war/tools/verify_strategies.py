"""Real-window fault tests and detached receiver proof; no quality work in live runs."""
from pathlib import Path
import argparse,subprocess,shutil,csv,json
from strategy_compare import settings,rows
from run_strategies import run
p=argparse.ArgumentParser();p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
root=a.root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
exe=root/'cw_live.exe';verify=root/'cw_wire_verify.exe';results=[]
def case(name,profile,extra):
    dest=out/name
    run([exe,'--input',root/'素材/car_raw.bgr','--out',dest,'--profile',root/'profiles'/f'{profile}.ini','--duration','5','--autostart','--close-on-end',*extra],out/(name+'.log'),root,120)
    run([verify,'--fast',dest],out/(name+'-verify.log'),root,120)
    s=settings(dest/'summary.txt');assert s['error']=='',s['error']
    assert int(s['decoded_unassociated'])==0
    assert int(s['max_sent_frame_packets'])<=2 and int(s['max_sent_frame_h264_bytes'])<=592
    results.append(dict(name=name,profile=profile,summary=s));print('PASS',name,flush=True);return dest,s
for profile in ['one-320','one-320-aq0','one-320-gop20','upscale','gray']:
    dest,s=case('all-drop-'+profile,profile,['--test-drop-all'])
    assert int(s['source_frames'])>80 and int(s['rx_packets'])==int(s['decoded'])==int(s['displayed_or_headless_consumed'])==0
    assert not (dest/'last-received.png').exists()
for profile in ['one-320','one-320-gop20']:
    # First establish the deterministic packet index for the SAME source frame.
    ref,s=case('loss-control-'+profile,profile,[])
    packet=next(r['packet'] for r in rows(ref/'tx-times.csv') if r['source_frame']=='25')
    dest,s=case('loss-'+profile,profile,['--drop-packet',packet])
    dropped=[r for r in rows(dest/'tx-times.csv') if r['status']=='network-drop']
    assert len(dropped)==1 and dropped[0]['source_frame']=='25',dropped
    assert int(s['decoded'])>40 and int(s['network_drop'])==1
    case('oversize-'+profile,profile,['--test-oversize-frame','1'])
for stage in ['pre','encode','post']:
    dest,s=case(stage+'-load','one-320-aq0',['--test-'+stage+'-ms','100'])
    assert int(s['source_frames'])>=95
    if stage!='post':assert int(s['encode_overwrite'])>0
post,s=case('reconstruction','upscale',[])
# Detached replay includes receiver processing, but has no source or observation IDs.
detached=out/'detached-post';detached.mkdir()
for name in ['rx-300.bin','rx-times.csv','decoded.csv','processed.csv','receiver.ini']:
    shutil.copyfile(post/name,detached/name)
run([verify,'--rx-only','--fast',detached],out/'detached-post.log',root,120)
for name,change in [('bad-algorithm',lambda d:(d/'receiver.ini').write_text('upscale=1\nsharpen=0\n',encoding='utf-8')),
                    ('source-side-channel',lambda d:(d/'receiver.ini').write_text('input=car_raw.bgr\n',encoding='utf-8'))]:
    dest=out/name;shutil.copytree(detached,dest);change(dest)
    with (out/(name+'.log')).open('w',encoding='utf-8') as f:r=subprocess.run([str(verify),'--rx-only','--fast',str(dest)],cwd=root,stdout=f,stderr=subprocess.STDOUT)
    assert r.returncode!=0,name
# Reopen the same temporal strategy: causal history must reset, no singleton stale pixels.
first,_=case('temporal-first','temporal16',[]);second,_=case('temporal-reopen','temporal16',[])
assert rows(first/'processed.csv')[0]['sha256']==rows(second/'processed.csv')[0]['sha256']
legacy=out/'accepted-baseline';shutil.copytree(root/'evidence/two-packet-baseline',legacy)
run([verify,'--fast',legacy],out/'accepted-baseline.log',root,120)
(out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')
print('PASS strategy faults, postprocessing replay, tamper rejection, reset and preserved baseline',flush=True)
