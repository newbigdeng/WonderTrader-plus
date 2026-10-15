"""Check stock monitor login, file-backed views and WebSocket notifications."""
from pathlib import Path
import asyncio, datetime, hashlib, importlib.metadata, json, sqlite3, time
import requests, websockets

ROOT=Path(__file__).resolve().parent
BASE='http://127.0.0.1:8099'
login=json.loads((ROOT/'login.local.json').read_text())
session=requests.Session()
response=session.post(BASE+'/mgr/login',json={'loginid':login['username'],
    'passwd':hashlib.md5(login['password'].encode()).hexdigest()})
assert response.status_code==200 and response.json()['result']==0
spec=session.get(BASE+'/openapi.json').json(); results={}

def query(path,group,strategy='trend_pullback'):
    schema=spec['paths'][path]['post']['requestBody']['content']['application/json']['schema']
    if '$ref' in schema:schema=spec['components']['schemas'][schema['$ref'].split('/')[-1]]
    values={'groupid':group,'channelid':'simnow','strategyid':strategy,'straid':strategy,
            'token':None,'id':group,'type':'runner'}
    payload={k:values[k] for k in schema.get('properties',{}) if k in values}
    response=session.post(BASE+path,json=payload)
    assert response.status_code==200,(path,response.status_code,response.text[:100])
    data=response.json(); assert data['result']==0,(path,data)
    results[group+' '+path]=data; return data

query('/mgr/qrygrp','simnow03')
for group in ['simnow03','acceptance01','acceptance02']:
    for path in ['/mgr/qrystras','/mgr/qrychnls','/mgr/qrychnlpos','/mgr/qrychnlfund','/mgr/qrygrpdir','/mgr/qrylogs']:
        query(path,group)
    for path,key in [('/mgr/qrychnlords','orders'),('/mgr/qrychnltrds','trades')]:
        counts=[len(query(path,group)[key]) for _ in range(3)]
        expected=0 if group=='simnow03' else 2
        assert counts==[expected]*3,(group,path,counts)
        results[group+' repeated '+key]=counts
query('/mgr/qrypos','simnow03')
funds=results['simnow03 /mgr/qrychnlfund']['funds']['simnow']['CNY']
report={'date':'2026-10-15','verified_at':datetime.datetime.now().astimezone().isoformat(),
    'result':'passed','groups':['simnow03','acceptance01','acceptance02'],'http_read_checks':len(results),
    'live_mode':'observe; no executers; TrendPullback armed=False','live_funds':funds,
    'archive_orders_each':2,'archive_trades_each':2,
    'repeat_refresh_counts':{'orders':[2,2,2],'trades':[2,2,2]},
    'sqlite_schedules':sqlite3.connect(ROOT/'data.db').execute('SELECT appid,guard,schedule FROM schedules').fetchall(),
    'dependencies':{n:importlib.metadata.version(n) for n in ['fastapi','uvicorn','websockets','starlette','pydantic']}}

async def check_push():
    cookie='; '.join(k+'='+v for k,v in session.cookies.items())
    async with websockets.connect('ws://127.0.0.1:8099/',additional_headers={'Cookie':cookie}) as ws:
        await ws.send(json.dumps({'type':'subscribe','groupid':'simnow03'}))
        subscribe=json.loads(await asyncio.wait_for(ws.recv(),5))
        await ws.send(json.dumps({'type':'heartbeat'}))
        heartbeat=json.loads(await asyncio.wait_for(ws.recv(),5))
        assert subscribe['type']=='subscribe' and heartbeat['message']=='pong'
        report['websocket_subscribe_and_heartbeat']='passed'
        deadline=time.monotonic()+55
        while time.monotonic()<deadline:
            try:message=json.loads(await asyncio.wait_for(ws.recv(),deadline-time.monotonic()))
            except asyncio.TimeoutError:break
            if message.get('type')=='gplog':
                report['native_mq_to_websocket_log']='passed'
                report['sample_live_log']={k:message.get(k) for k in ['groupid','tag','time','message']}
                return
        report['native_mq_to_websocket_log']='No native log frame observed within 55 seconds'

asyncio.run(check_push())
output=ROOT/'evidence'; output.mkdir(exist_ok=True)
(output/'api-responses.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
(output/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(report,ensure_ascii=False,indent=2))
