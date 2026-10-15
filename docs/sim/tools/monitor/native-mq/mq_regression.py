"""Compare actual old/new C++ notifier builds through the actual WtMsgQue client."""
from pathlib import Path
import sys, subprocess, time, json, collections
root=Path('/home/zjh/wt-lab/records/simnow-03')
sys.path.insert(0,str(root/'runtime'))
from wtpy import WtMsgQue,WtMQClient
e=root/'monitor/evidence/mq-fix-20261015'

class Recorder(WtMQClient):
    def __init__(self): self.records=[]
    def on_mq_message(self, topic, message, dataLen):
        self.records.append({'received_at':time.time(),'topic':topic.decode(),
                             'data':json.loads(message[:dataLen].decode())})

results={}
mq=WtMsgQue()
for version in ['before','after']:
    commands=e/(version+'-commands.txt');commands.write_text('')
    url='ipc://'+str(e/(version+'.ipc'))
    with (e/(version+'-native.stdout.log')).open('w') as out:
        p=subprocess.Popen([str(e/('native-mq-'+version)),url,str(root/'runtime/wtpy/wrapper/linux')+'/',str(commands)],stdout=out,stderr=subprocess.STDOUT)
        time.sleep(1)
        recorder=Recorder();mq.add_mq_client(url,recorder);recorder.start();time.sleep(1)
        for text in ['LOG MQ_IDLE_FIRST','ORDER\nCANCEL\nTRADE','LOG MQ_IDLE_SECOND']:
            with commands.open('a') as cmd: cmd.write(text+'\n')
            time.sleep(2)
        with commands.open('a') as cmd:cmd.write('QUIT\n')
        began=time.monotonic();code=p.wait(timeout=4);shutdown=time.monotonic()-began
        mq.destroy_mq_client(recorder)
    counts=dict(collections.Counter(x['topic'] for x in recorder.records))
    (e/(version+'-raw.json')).write_text(json.dumps(recorder.records,indent=2)+'\n')
    stdout=(e/(version+'-native.stdout.log')).read_text()
    refs=[int(s.split()[1]) for s in stdout.splitlines() if s.startswith('ORDER_REFS')]
    results[version]={'topics':counts,'order_ref_counts':refs,'exit_code':code,
                      'shutdown_seconds':shutdown,'destructor_completed':'NOTIFIER_DESTROYED' in stdout}
assert not results['before']['topics'],results
assert results['before']['order_ref_counts']==[2,2],results
assert results['after']['topics']=={'LOG':2,'TRD_ORDER':2,'TRD_TRADE':1},results
assert results['after']['order_ref_counts']==[1,1],results
assert all(x['exit_code']==0 and x['destructor_completed'] for x in results.values())
(e/'native-regression.json').write_text(json.dumps({'result':'passed','cases':results},indent=2)+'\n')
print(json.dumps(results))
