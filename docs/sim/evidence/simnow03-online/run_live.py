"""Prepared CTP launch. This script is not executed during offline validation."""
import argparse,json,os,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
os.chdir(Path(__file__).resolve().parent)
sys.path[:0]=[str(ROOT/'runtime'),str(ROOT)]
os.environ['TZ']='Asia/Shanghai'
time.tzset()
from wtpy import WtEngine,EngineType
from strategies.TrendPullback import TrendPullback
from account_checks import position_volume

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--trade',action='store_true',help='Enable strategy targets after fresh broker-state checks')
    parser.add_argument('--account-checked',action='store_true',help='Operator has reconciled funds, positions and unfinished orders in the official terminal')
    parser.add_argument('--state-dir',default='generated',help='Runtime state directory; trade startup requires it to be empty')
    args=parser.parse_args()
    if args.trade and not args.account_checked:parser.error('--trade also requires --account-checked; verify unfinished orders in the official terminal')
    state_dir=Path(args.state_dir).resolve()
    if state_dir.parent!=Path.cwd():parser.error('--state-dir must be a direct child directory of run')
    if args.trade and state_dir.exists() and any(state_dir.rglob('*')):
        parser.error('Trade startup requires a fresh state directory: native restored targets may act before Python arming. Reconcile the account, then choose a new state directory; never delete a live position to bypass this check.')
    settings=json.loads((ROOT/'strategy.json').read_text())
    strategy=TrendPullback('trend_pullback',**settings,armed=False)
    config='config.ctp-trade.yaml' if args.trade else 'config.ctp-observe.yaml'
    engine=WtEngine(EngineType.ET_CTA,'logcfg.yaml',genDir=str(state_dir))
    engine.init('../common/',config)
    engine.commitConfig()
    engine.add_cta_strategy(strategy)
    started=time.time();engine.run(True)
    snapshot=state_dir/'traders/simnow/rtdata.json'
    print('CTP started; --trade requires fresh CNY funds and an empty broker position list.',flush=True)
    enabled=False
    try:
        while True:
            if snapshot.exists() and snapshot.stat().st_mtime>=started:
                try:
                    state=json.loads(snapshot.read_text())
                    fund=state.get('funds',{}).get('CNY')
                    if fund:
                        strategy.broker_available=float(fund['available'])
                        strategy.broker_updated_at=snapshot.stat().st_mtime
                        if args.trade and not enabled:
                            if time.time()-strategy.broker_updated_at>180:
                                raise RuntimeError('Broker account snapshot is stale')
                            if position_volume(state)>0:
                                raise RuntimeError('Broker positions are not empty; do not start a new strategy against them')
                            if not __import__('math').isfinite(strategy.broker_available) or strategy.broker_available < settings['capital']*settings['margin_cap']+100:
                                raise RuntimeError('Insufficient broker available funds for the configured budget')
                            strategy.armed=True;enabled=True
                            print('Fresh snapshot checked; strategy is armed with a maximum one-lot target.',flush=True)
                except json.JSONDecodeError:
                    pass  # Native writer may be replacing the JSON while we read it.
            time.sleep(1)
    except KeyboardInterrupt:
        print('Stop requested. Check the official terminal for orders/positions; native WtPorter complete shutdown is not yet verified.',flush=True)
    finally:
        engine.release()

if __name__=='__main__':main()
