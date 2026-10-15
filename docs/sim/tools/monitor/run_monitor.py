"""Launch the bundled wtpy monitor against simnow-03's registered groups."""
from pathlib import Path
import os, sys, time, fcntl

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
os.chdir(HERE)
os.umask(0o077)
lock = (HERE / '.monitor.lock').open('a')
try:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    raise SystemExit('Monitor already running (lock held).')
(HERE / 'monitor.pid').write_text(str(os.getpid()))
os.environ['TZ'] = 'Asia/Shanghai'
time.tzset()
sys.path.insert(0, str(ROOT / 'runtime'))
from wtpy.monitor import WtMonSvr

server = WtMonSvr(deploy_dir=str(ROOT), notifyTimeout=False)
server.run(port=8099, host='127.0.0.1', bSync=True)
