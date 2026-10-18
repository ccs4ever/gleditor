from pathlib import Path
import re,hashlib,json
base=Path('build/validation-sixteenth/native')
perma=(base/'data/xuzz/permascroll/default/active.primedia').read_bytes()
out=[]
for p in sorted(base.rglob('activity.ops.txt')):
 if '/checkpoints/' not in str(p):continue
 text=p.read_text();cells={};edges={};dims={};subjects={}
 for line in text.splitlines():
  m=re.match(r'op (\d+) .*?span=\[(\d+),(\d+)\)',line)
  if not m:continue
  op,a,b=map(int,m.groups())
  if '[make cell' in line:
   content=perma[a:b].decode('utf-8');cells[op]=content;subjects[op]=op
   if content.startswith('d.activity-'):dims[content]=op
  link=re.search(r'\[setLink posward dim=(\d+) -> (\d+) cell@(\d+)',line)
  if link:
   dim,target,previous=map(int,link.groups());subject=subjects[previous]
   subjects[op]=subject;edges[subject,dim]=target
 def rank(name):
  dim=dims.get(name);pos=2;seen=set();values=[]
  while dim is not None and (pos,dim) in edges:
   pos=edges[pos,dim]
   assert pos not in seen
   seen.add(pos);values.append(cells[pos])
  return values
 visits=rank('d.activity-visits');current=rank('d.activity-current')
 source={q.name:hashlib.sha256(q.read_bytes()).hexdigest() for q in p.parent.glob('source-*.ops.txt')}
 out.append({'checkpoint':str(p.parent.relative_to(base)),'visit_count':len(visits),'current':int(current[-1]) if current else None,'visits':[{'id':i+1,'payload':v,'parent':int(v.split()[1]) or None} for i,v in enumerate(visits)],'references':rank('d.activity-references'),'annotations':rank('d.activity-annotations'),'source_ops_sha256':source})
Path('build/validation-sixteenth/native/evidence/decoded-checkpoints.json').write_text(json.dumps(out,indent=2,ensure_ascii=False)+'\n')
for item in out:
 print(item['checkpoint'], 'visits',item['visit_count'],'current',item['current'],'references',item['references'],'annotations',item['annotations'])
