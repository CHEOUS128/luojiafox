"""Optional full-resolution LEFT-panel export; never used by the receiver or quality score."""
from pathlib import Path
import argparse,json,subprocess,shutil
from PIL import Image
from strategy_compare import digest,rows
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--input',type=Path,required=True);p.add_argument('--ffmpeg',type=Path,required=True);a=p.parse_args()
manifest=json.loads((a.root/'experiment.json').read_text(encoding='utf-8'))
assert digest(a.input)==manifest['input_sha256'],'source SHA mismatch'
dest=a.root/'original-native';dest.mkdir(exist_ok=False)
sources={int(s['file_index']):(int(s['width']),int(s['height'])) for d in a.root.iterdir() if (d/'source-map.csv').exists() for s in rows(d/'source-map.csv')}
if a.input.suffix.lower() in ['.bgr','.raw','.bin']:
    with a.input.open('rb') as f:
        for i,(w,h) in sorted(sources.items()):
            f.seek(i*w*h*3);b=f.read(w*h*3);assert len(b)==w*h*3
            Image.frombytes('RGB',(w,h),b,'raw','BGR').save(dest/f'{i}.png',compress_level=1)
else:
    subprocess.run([str(a.ffmpeg),'-hide_banner','-loglevel','error','-i',str(a.input),'-map','0:v:0','-fps_mode','passthrough','-start_number','0',str(dest/'%d.png')],check=True)
path=a.root/'comparison-data.js';data=json.loads(path.read_text(encoding='utf-8')[len('window.COMPARISON='):-1]);data['nativeOriginal']=True
path.write_text('window.COMPARISON='+json.dumps(data,ensure_ascii=False)+';',encoding='utf-8')
shutil.copyfile(Path(__file__).with_name('strategy_viewer.html'),a.root/'index.html')
print('Native original LEFT only:',dest)
