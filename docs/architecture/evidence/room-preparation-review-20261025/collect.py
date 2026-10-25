import argparse, hashlib, json, math, os, platform, shutil, struct, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('suite',type=Path);p.add_argument('build',type=Path);p.add_argument('output',type=Path);p.add_argument('label',choices=['baseline','candidate']);a=p.parse_args()
suite=a.suite.resolve();build=a.build.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
cat=json.loads((suite/'catalog.json').read_text());suffix='.exe' if os.name=='nt' else ''
def exe(name):return build/(name+suffix)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def run(args,name):
 with (out/(name+'.log')).open('wb') as f:r=subprocess.run([str(v) for v in args],stdout=f,stderr=subprocess.STDOUT)
 if r.returncode:raise RuntimeError(name+': '+(out/(name+'.log')).read_text(encoding='utf-8',errors='replace')[-3000:])
def pcm(path):
 data=path.read_bytes();magic,version,ch,rate,frames=struct.unpack('<4sIIIQ',data[:24]);assert magic==b'MRPB' and version==1 and len(data)==24+frames*ch*4
 assert all(math.isfinite(v[0]) for v in struct.iter_unpack('<f',data[24:]))
 return {'shape':[ch,rate,frames],'sha256':hashlib.sha256(data[24:]).hexdigest()}
def work():
 path=out/'work'
 if path.exists():shutil.rmtree(path)
 path.mkdir();return path
def save(): (out/'record.json').write_text(json.dumps(record,indent=2)+'\n')
record={'revision':cat[a.label],'platform':platform.platform(),'machine':platform.machine(),'catalog_sha256':sha(suite/'catalog.json'),'binaries':{},'fixtures':{},'offline':{},'stream':{},'scene':{},'fenv':{},'complete':False}
for name in ['mradm','mr_adm_make_fixture','mr_adm_phase2_stream','mr_adm_phase2_render','mr_adm_phase2_scene','mr_adm_pcm_bits','mradm_review_room_fenv']:record['binaries'][name]=sha(exe(name))
fixtures={r['fixture'] for r in cat['offline']}|{r['case']['fixture'] for r in cat['stream']}|set(cat['fenv'])
for name in sorted(fixtures):
 path=suite/'fixtures'/(name+'.wav')
 if a.label=='baseline':run([exe('mr_adm_make_fixture'),name,path],'fixture-'+name)
 record['fixtures'][name+'.wav']=sha(path)
for path in sorted((suite/'fixtures').rglob('*')):
 if path.is_file():record['fixtures'][path.relative_to(suite/'fixtures').as_posix()]=sha(path)
save()
for row in cat['offline']:
 name=row['id'];print(a.label,'offline',name,flush=True);temp=work()
 run([exe('mr_adm_phase2_render'),suite/'cases'/(name+'.json'),suite/'fixtures'/(row['fixture']+'.wav'),temp],name)
 first,second=temp/'pass-1.pcmbits',temp/'pass-2.pcmbits';assert first.read_bytes()==second.read_bytes(),name+' repeat'
 result=pcm(first)
 args=row['args'];depth=[] if '--output-bit-depth' in args else ['--output-bit-depth','f32']
 files=[v for flag,path in row.get('files',{}).items() for v in [flag,suite/'fixtures'/path]]
 if 'sofa' in row:files+=['--sofa',suite/'fixtures'/row['sofa']]
 run([exe('mradm'),'render','-i',suite/'fixtures'/(row['fixture']+'.wav'),'-o',temp/'cli.wav',*depth,*args,*files],name+'-cli')
 run([exe('mr_adm_pcm_bits'),'extract',temp/'cli.wav',temp/'cli.pcmbits'],name+'-extract')
 assert first.read_bytes()==(temp/'cli.pcmbits').read_bytes(),name+' CLI versus API'
 record['offline'][name]=result;shutil.rmtree(temp);save()
for row in cat['stream']:
 case=row['case'];name=case['id'];print(a.label,'stream',name,flush=True);temp=work()
 run([exe('mr_adm_phase2_stream'),suite/'cases'/(name+'.json'),suite/'fixtures'/(case['fixture']+'.wav'),temp],name)
 first,second=temp/'pass-1.pcmbits',temp/'pass-2.pcmbits';assert first.read_bytes()==second.read_bytes(),name+' repeat'
 result=pcm(first);assert result['shape']==row['expected_shape'],name+' timeline'
 record['stream'][name]=result;shutil.rmtree(temp);save()
for row in cat['scene']:
 name=row['id'];print(a.label,'Scene',name,flush=True);temp=work()
 run([exe('mr_adm_phase2_scene'),suite/'cases'/(name+'.json'),temp],name)
 states=[json.loads((temp/f'pass-{n}.json').read_text()) for n in [1,2]];assert states[0]==states[1] and states[0]['status']=='passed'
 result={'status':states[0],'epochs':{},'input':sha(temp/'input.pcmbits'),'events':sha(temp/'events.json')}
 for epoch in [1,2]:
  first,second=[temp/f'pass-{n}-epoch-{epoch}.pcmbits' for n in [1,2]];assert first.read_bytes()==second.read_bytes(),name+' repeat'
  result['epochs'][str(epoch)]=pcm(first)
 record['scene'][name]=result;shutil.rmtree(temp);save()
for fixture in cat['fenv']:
 print(a.label,'FENV',fixture,flush=True);temp=work()
 run([exe('mradm_review_room_fenv'),suite/'fixtures'/(fixture+'.wav'),suite/'fixtures/room/room.json',temp],'fenv-'+fixture)
 result={'status':json.loads((temp/'status.json').read_text()),'pcm':{}}
 for rounding in range(4):
  for flush in range(2):
   name=f'round-{rounding}-flush-{flush}'
   first,second=[temp/(name+f'-pass-{n}.pcmbits') for n in [1,2]];assert first.read_bytes()==second.read_bytes(),name+' repeat'
   result['pcm'][name]=pcm(first)
 record['fenv'][fixture]=result;shutil.rmtree(temp);save()
record['complete']=True;save()
if a.label=='candidate':
 before=json.loads((out.parent/'baseline/record.json').read_text());assert before['complete']
 checks={key:before[key]==record[key] for key in ['catalog_sha256','fixtures','offline','stream','scene','fenv']}
 report={'passed':all(checks.values()),'baseline':cat['baseline'],'candidate':cat['candidate'],'platform':record['platform'],'checks':checks,'offline_cli_cases':len(record['offline']),'stream_cases':len(record['stream']),'scene_cases':len(record['scene']),'scene_pcm_epochs':2*len(record['scene']),'fenv_cases':8*len(record['fenv']),'repeat_passes':2}
 (out.parent/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');assert report['passed'],checks
print(a.label,'complete',flush=True)
