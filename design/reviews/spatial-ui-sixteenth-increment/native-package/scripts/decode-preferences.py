from pathlib import Path
import json,hashlib
base=Path('build/validation-sixteenth/native/evidence')
def decode(data):
 pos=0
 def value():
  nonlocal pos
  ch=data[pos:pos+1];pos+=1
  if ch==b'i':
   end=data.index(b'e',pos);v=int(data[pos:end]);pos=end+1;return v
  if ch in (b'd',b'l'):
   entries=[]
   while data[pos:pos+1]!=b'e':entries.append(value())
   pos+=1
   return dict(zip(entries[::2],entries[1::2])) if ch==b'd' else entries
  start=pos-1;end=data.index(b':',start);length=int(data[start:end]);pos=end+1
  v=data[pos:pos+length].decode();pos+=length;return v
 result=value();assert pos==len(data);return result
rows=[]
for p in sorted((base/'checkpoints').glob('*/package-visibility.bencode.txt')):
 v=decode(p.read_bytes());assert v['format']==2
 rows.append({'checkpoint':p.parent.name,'preference_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'preference':v['choices']})
(base/'decoded-preferences.json').write_text(json.dumps(rows,indent=2)+'\n')
for row in rows:print(row['checkpoint'], row['preference'])
activity=json.loads((base/'decoded-checkpoints.json').read_text())
assert all(x['visit_count']==1 and x['current']==1 for x in activity)
assert len({tuple(x['source_ops_sha256'].values()) for x in activity})==1
filehashes={tuple(line.split()[0] for line in p.read_text().splitlines()) for p in (base/'checkpoints').glob('*/source-0.files.sha256')}
assert len(filehashes)==1
print('Native visit count/current and source operation/file invariants verified')
