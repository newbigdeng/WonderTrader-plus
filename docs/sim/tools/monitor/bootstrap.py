"""Register monitor users/groups and an observation-only managed process."""
from pathlib import Path
import os, sys, json, secrets, hashlib, shutil

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
os.chdir(HERE)
os.umask(0o077)
sys.path.insert(0, str(ROOT / 'runtime'))
from wtpy.monitor.DataMgr import DataMgr
from wtpy.monitor.WatchDog import WatchDog
from wtpy.monitor.WtLogger import WtLogger

credentials = HERE / 'login.local.json'
if not credentials.exists():
    credentials.write_text(json.dumps({'username':'localadmin', 'password':secrets.token_urlsafe(15)}, indent=2)+'\n')
    credentials.chmod(0o600)
login = json.loads(credentials.read_text())
manager = DataMgr('data.db')
if manager.get_user(login['username']) is None:
    manager.add_user({'loginid':login['username'], 'name':'simnow-03本机管理员', 'role':'superman',
        'passwd':hashlib.md5(login['password'].encode('utf-8')).hexdigest(),
        'iplist':'', 'products':'', 'remark':'本机SSH转发访问'}, 'bootstrap')

groups = [
    {'id':'simnow03', 'name':'simnow-03 实时观察', 'path':str(ROOT/'run'),
     'info':'当前观察模式：原TrendPullback接收行情，执行器为空；通道simnow显示柜台快照',
     'gtype':'cta', 'datmod':'auto', 'env':'product', 'mqurl':'ipc://'+str(ROOT/'run/monitor.ipc')},
]
for ident, state, label in [
    ('acceptance01','generated_online_roundtrip_01','验收档案① 开仓后恢复平仓'),
    ('acceptance02','generated_online_roundtrip_03','验收档案② 连续单手开平'),
]:
    folder = HERE / 'views' / ident
    folder.mkdir(parents=True, exist_ok=True)
    target = ROOT/'run'/state
    assert (target/'marker.json').exists(), 'Missing genuine archive state'
    link = folder/'generated'
    if link.is_symlink():
        assert link.resolve() == target.resolve()
        link.unlink()  # Remove our view link only; preserve the original run state.
    if not link.exists():
        shutil.copytree(target, link)
    assert (link/'marker.json').exists()
    (folder/'config.yaml').write_text('# Monitor archive index, not historical trading config.\narchive_only: true\nparsers: []\ntraders: []\nexecuters: []\n')
    (folder/'run.py').write_text('raise SystemExit("Historical archive viewer; no strategy launcher configured.")\n')
    logsource = ROOT/'evidence/online-20261014'/('recovery' if ident=='acceptance01' else 'roundtrip3')/'logs/Logs'
    if logsource.exists() and not (folder/'Logs').exists():
        (folder/'Logs').symlink_to(logsource,target_is_directory=True)
    groups.append({'id':ident, 'name':label, 'path':str(folder), 'gtype':'cta',
        'datmod':'mannual', 'env':'product', 'mqurl':'',
        'info':'2026-10-14真实SimNow模拟成交快照副本，不配置启动程序；资金是当时快照'})
for group in groups:
    assert manager.add_group(group), group['id']

watcher = WatchDog(manager.get_db(), logger=WtLogger('bootstrap', 'bootstrap.log'))
tasks = [{'active':False, 'time':0, 'action':0} for _ in range(6)]
watcher.applyAppConf({'id':'simnow03', 'path':'/home/zjh/anaconda3/bin/python', 'folder':str(ROOT/'run'),
    'param':'run.py', 'span':3, 'guard':False, 'redirect':False,
    'mqurl':groups[0]['mqurl'], 'schedule':{'active':False,'weekflag':'0111110','tasks':tasks}}, isGroup=True)
(HERE/'groups.json').write_text(json.dumps(groups,ensure_ascii=False,indent=2)+'\n')
print('Registered 3 groups; automatic scheduling and process guarding are disabled.')
