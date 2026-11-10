"""Sequential real-window experiments; no simultaneous encoder competition."""
from pathlib import Path
import argparse,subprocess,json,time,hashlib,sys
from strategy_compare import build_comparison,digest
if hasattr(sys.stdout,'reconfigure'):sys.stdout.reconfigure(encoding='utf-8')

def run(cmd,log,cwd,timeout):
    with log.open('w',encoding='utf-8') as f:
        f.write(json.dumps([str(x) for x in cmd],ensure_ascii=False)+'\n');f.flush()
        result=subprocess.run([str(x) for x in cmd],cwd=cwd,stdout=f,stderr=subprocess.STDOUT,timeout=timeout,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if result.returncode:raise RuntimeError(f'exit={result.returncode}: {log}')

def main():
    p=argparse.ArgumentParser();p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--input',type=Path);p.add_argument('--profiles',nargs='*');p.add_argument('--duration',type=float,default=15);p.add_argument('--repeat',type=int,default=1)
    p.add_argument('--once',action='store_true',help='play the complete file once; overrides duration')
    p.add_argument('--headless',action='store_true');p.add_argument('--no-compare',action='store_true');p.add_argument('--no-export',action='store_true');p.add_argument('--append',action='store_true');p.add_argument('--extra',nargs=argparse.REMAINDER,default=[]);a=p.parse_args()
    root=a.root.resolve();out=a.out.resolve();source=(a.input or root/'素材/car_raw.bgr').resolve()
    if out.exists() and not a.append:raise RuntimeError('output exists; choose a fresh experiment directory or explicitly --append')
    if a.no_export and not a.no_compare:raise RuntimeError('--no-export requires --no-compare')
    if not a.once and a.duration<=0:raise RuntimeError('looped tests require a positive duration; use --once for the complete file')
    out.mkdir(parents=True,exist_ok=a.append);profiles=a.profiles or [s.stem for s in sorted((root/'profiles').glob('*.ini'))]
    manifest=dict(input=str(source),input_sha256=digest(source),ffmpeg_sha256=digest(root/'ffmpeg.exe'),live_sha256=digest(root/'cw_live.exe'),verifier_sha256=digest(root/'cw_wire_verify.exe'),headless=a.headless,runs=[])
    manifest['source_files']={str(p.relative_to(root)).replace('\\','/'):digest(p) for folder in ['src','live_rx','tests'] for p in sorted((root/folder).rglob('*')) if p.is_file() and p.suffix in ['.cpp','.h']}
    if a.append:
        old=json.loads((out/'experiment.json').read_text(encoding='utf-8'))
        for k in ['input_sha256','ffmpeg_sha256','live_sha256','verifier_sha256','headless']:
            if old[k]!=manifest[k]:raise RuntimeError(f'cannot append different experiment environment: {k}')
        manifest['runs']=old['runs']
    for rep in range(a.repeat):
        for name in profiles:
            profile=root/'profiles'/f'{name}.ini'
            if not profile.exists():raise RuntimeError(f'profile does not exist: {profile}')
            dest=out/(name if a.repeat==1 else f'{name}-repeat{rep+1}')
            if dest.exists():raise RuntimeError(f'run already exists: {dest}')
            cmd=[root/'cw_live.exe','--input',source,'--profile',profile,'--offline-metrics','--duration',str(0 if a.once else a.duration),'--out',dest]
            if not a.once:cmd+=['--loop']
            cmd+=['--headless'] if a.headless else ['--autostart','--close-on-end'];cmd+=a.extra
            print('RUN',dest.name,flush=True)
            run(cmd,out/(dest.name+'-live.log'),root,None if a.once else max(120,a.duration*4+30))
            verify=[root/'cw_wire_verify.exe','--fast']+([] if a.no_export else ['--export'])+[dest]
            run(verify,out/(dest.name+'-verify.log'),root,max(120,a.duration*3))
            manifest['runs'].append(dict(directory=dest.name,profile_sha256=digest(profile),profile=str(profile)))
            (out/'experiment.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
            from strategy_compare import settings
            summary=settings(dest/'summary.txt')
            print('PASS wire and reconstruction',dest.name,'; actual display',summary['display_acceptance'],'min',summary['minimum_stable_display_fps'],'fps',flush=True)
    if not a.no_compare:build_comparison(out,source,root/'ffmpeg.exe')

if __name__=='__main__':main()
