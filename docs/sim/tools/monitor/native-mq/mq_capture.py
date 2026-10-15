"""Read-only monitor WebSocket recorder; no engine or trading API calls."""
import asyncio, hashlib, json, sys, time
from pathlib import Path
import requests, websockets

root=Path('/home/zjh/wt-lab/records/simnow-03')
e=root/'monitor/evidence/mq-fix-20261015'
label=sys.argv[1]; group=sys.argv[2] if len(sys.argv)>2 else 'simnow03'
duration=float(sys.argv[3]) if len(sys.argv)>3 else 65
credentials=json.loads((root/'monitor/login.local.json').read_text())
session=requests.Session()
r=session.post('http://127.0.0.1:8099/mgr/login',json={
    'loginid':credentials['username'],
    'passwd':hashlib.md5(credentials['password'].encode()).hexdigest()},timeout=10)
r.raise_for_status(); assert r.json()['result']==0,r.text
cookie='; '.join(k+'='+v for k,v in session.cookies.items())

async def main():
    records=[]
    async with websockets.connect('ws://127.0.0.1:8099/',additional_headers={'Cookie':cookie}) as ws:
        await ws.send(json.dumps({'type':'subscribe','groupid':group}))
        deadline=time.time()+duration
        with (e/(label+'.jsonl')).open('w') as out:
            while time.time()<deadline:
                try:
                    raw=await asyncio.wait_for(ws.recv(),timeout=min(10,deadline-time.time()))
                except asyncio.TimeoutError:
                    await ws.send(json.dumps({'type':'heartbeat'}));continue
                item={'received_at':time.time(),'frame':json.loads(raw)}
                records.append(item);out.write(json.dumps(item,ensure_ascii=False)+'\n');out.flush()
    counts={}
    for x in records:
        f=x['frame'];key=f.get('type','?')+':'+f.get('evttype','')
        counts[key]=counts.get(key,0)+1
    result={'group':group,'duration':duration,'counts':counts,'records':len(records)}
    (e/(label+'-summary.json')).write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))
asyncio.run(main())
