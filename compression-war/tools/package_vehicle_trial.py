"""Package only explicitly changed Jetson source files; never connect to the vehicle."""
from pathlib import Path
import argparse,hashlib,zipfile,json,sys
if hasattr(sys.stdout,'reconfigure'):sys.stdout.reconfigure(encoding='utf-8')
p=argparse.ArgumentParser();p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2]);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
upper=a.repo/'autoaim/my_lf_INF_20261025'
files=['io/video_sender/video_sender.hpp','io/video_sender/video_sender.cpp','io/video_sender/cw_trial.hpp','io/video_sender/cw_tx_worker.hpp',
       'io/gimbal/gimbal.hpp','io/gimbal/gimbal.cpp','src/standard_mpc.cpp','tests/video_sender_test.cpp']
if a.out.exists():raise RuntimeError('refusing to overwrite a previous transfer package')
a.out.parent.mkdir(parents=True,exist_ok=True);manifest=[]
with zipfile.ZipFile(a.out,'w',zipfile.ZIP_DEFLATED) as z:
    for name in files:
        b=(upper/name).read_bytes();sha=hashlib.sha256(b).hexdigest();manifest.append(dict(file=name,bytes=len(b),sha256=sha))
        z.writestr(name,b)
    z.writestr('SHA256SUMS',''.join(r['sha256']+'  '+r['file']+'\n' for r in manifest))
    z.writestr('manifest.json',json.dumps(dict(version='cw-trial-20261109-r2',status='B quality profile with gimbal TX DMA accepted in real vehicle use on 2026-11-12; package contains Jetson source only',files=manifest),ensure_ascii=False,indent=2))
    z.writestr('READ-ME.txt','For planned maintenance of the accepted B quality setup; retain existing models, calibration and configuration.\nBuild ONLY: make standard_mpc video_sender_test\nProfiles: baseline smooth detail detail-aq0 quality.\nNo profile preserves original YAML path.\nSee docs/2026-11-12-实车成功版本.md for the deployed service, manual ToDesk transfer and build procedure.\n')
sha=hashlib.sha256(a.out.read_bytes()).hexdigest();a.out.with_suffix('.sha256').write_text(sha+'  '+a.out.name+'\n',encoding='utf-8');print(a.out,sha)
