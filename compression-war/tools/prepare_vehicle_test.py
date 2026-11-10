"""Mechanically extract the production serial methods; stub only hardware/time IO."""
from pathlib import Path
import argparse,re,json,hashlib
p=argparse.ArgumentParser();p.add_argument('--vehicle',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
source=a.vehicle/'io/gimbal/gimbal.cpp';text=source.read_text(encoding='utf-8')
def function(name):
    start=text.index(name);brace=text.index('{',start)
    mask=re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',lambda m:' '*len(m[0]),text,flags=re.S);depth=0
    for i in range(brace,len(text)):
        depth+=(mask[i]=='{')-(mask[i]=='}')
        if depth==0:return text[start:i+1]
    raise ValueError(name)
names=['std::size_t Gimbal::write_some','void Gimbal::pump_writes','bool Gimbal::queue_control','bool Gimbal::try_send_raw','void Gimbal::enable_trial_video','bool Gimbal::try_send_trial_video','void Gimbal::service_trial_video','bool Gimbal::finish_trial_video']
(a.out/'vehicle-exact.inc').write_text('\n\n'.join(map(function,names)),encoding='utf-8',newline='\n')
(a.out/'source.json').write_text(json.dumps(dict(source=str(source.resolve()),sha256=hashlib.sha256(source.read_bytes()).hexdigest(),methods=names),indent=2),encoding='utf-8')
