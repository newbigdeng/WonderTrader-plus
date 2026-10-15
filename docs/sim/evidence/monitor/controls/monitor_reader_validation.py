"""Targeted monitor file-reader tests; controlled fixtures, no CTP connection.

Run on the VM using the monitor virtual environment. This imports the main wtpy
source, copies an existing historical archive into a temporary directory, and
never writes the simnow-03 runtime generated directory or its real database.
"""
from __future__ import annotations

import argparse
import importlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
from datetime import datetime


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', default='/home/zjh/wt-lab/src/wtpy')
    ap.add_argument('--archive', default='/home/zjh/wt-lab/records/simnow-03/run/generated_online_roundtrip_03')
    ap.add_argument('--output', required=True)
    args = ap.parse_args()
    sys.path.insert(0, args.source)

    dm_module = importlib.import_module('wtpy.monitor.DataMgr')
    wd_module = importlib.import_module('wtpy.monitor.WatchDog')
    DataMgr, WatchDog = dm_module.DataMgr, wd_module.WatchDog
    assert Path(dm_module.__file__).resolve().is_relative_to(Path(args.source).resolve())
    assert Path(wd_module.__file__).resolve().is_relative_to(Path(args.source).resolve())

    report = {
        'kind': 'controlled_fixture_monitor_reader_validation',
        'measured_at': datetime.now().astimezone().isoformat(),
        'source_data_mgr': str(Path(dm_module.__file__).resolve()),
        'source_watchdog': str(Path(wd_module.__file__).resolve()),
        'historical_archive_read_only': str(Path(args.archive).resolve()),
        'ctp_connected': False,
        'orders_sent': 0,
        'real_generated_modified': False,
        'real_monitor_database_modified': False,
        'processes_started_or_stopped': False,
        'hashes_computed': False,
        'checks': {},
    }

    class QuietLogger:
        def info(self, *args):
            pass
        def error(self, *args):
            pass

    with tempfile.TemporaryDirectory(prefix='wt-monitor-controlled-fixture-') as folder:
        fixture = Path(folder)
        generated = fixture / 'generated'
        shutil.copytree(args.archive, generated)
        marker = json.loads((generated / 'marker.json').read_text())
        channel = marker['channels'][0]
        strategy = marker['marks'][0]

        manager = DataMgr(str(fixture / 'controlled-fixture.db'))
        groupid = 'controlled_fixture'
        assert manager.add_group({
            'id': groupid, 'name': 'CONTROLLED FIXTURE ONLY',
            'path': str(fixture), 'info': 'Copied history plus artificial positions; never a real account',
            'gtype': 'cta', 'datmod': 'mannual', 'env': 'controlled_fixture_off_session', 'mqurl': '',
        })

        order_counts = [len(manager.get_channel_orders(groupid, channel)) for _ in range(3)]
        trade_counts = [len(manager.get_channel_trades(groupid, channel)) for _ in range(3)]
        assert order_counts == [2, 2, 2], order_counts
        assert trade_counts == [2, 2, 2], trade_counts
        report['checks']['repeated_historical_orders'] = {'counts': order_counts, 'passed': True}
        report['checks']['repeated_historical_trades'] = {'counts': trade_counts, 'passed': True}

        # All changes below are made only to the temporary copy, deliberately
        # labelled artificial. DataMgr has no wall-clock market-session gate.
        channel_path = generated / 'traders' / channel / 'rtdata.json'
        snapshot = json.loads(channel_path.read_text())
        snapshot.update({
            'controlled_fixture': True,
            'fixture_description': 'Artificial nonzero position for offline monitor reader only',
            'fixture_market_session': 'NON_TRADING',
            'trading_day': 20000101,
        })
        snapshot['positions'] = [{
            'code': 'SHFE.rb.2701',
            'long': {'newvol': 1, 'newavail': 1, 'prevol': 0, 'preavail': 0},
            'short': {'newvol': 0, 'newavail': 0, 'prevol': 0, 'preavail': 0},
        }]
        channel_path.write_text(json.dumps(snapshot, indent=2))

        detail = {'long': True, 'volume': 1, 'price': 100, 'opentime': 200001010900, 'profit': 0}
        engine_position = {'code': 'SHFE.rb.2701', 'volume': 1, 'details': [detail]}
        for path in (generated / 'stradata' / (strategy + '.json'), generated / 'portfolio' / 'datas.json'):
            path.parent.mkdir(parents=True, exist_ok=True)
            data = json.loads(path.read_text()) if path.exists() else {}
            data.update({
                'controlled_fixture': True,
                'fixture_market_session': 'NON_TRADING',
                'trading_day': 20000101,
                'positions': [engine_position],
            })
            path.write_text(json.dumps(data, indent=2))

        channel_positions = manager.get_channel_positions(groupid, channel)
        strategy_positions = manager.get_positions(groupid, strategy)
        portfolio_positions = manager.get_group_positions(groupid)
        channel_total = sum(sum(p[side][v] for v in ('newvol', 'prevol'))
                            for p in channel_positions for side in ('long', 'short'))
        strategy_total = sum(p['volume'] for p in strategy_positions)
        portfolio_total = sum(p['volume'] for p in portfolio_positions)
        assert [channel_total, strategy_total, portfolio_total] == [1, 1, 1]
        report['checks']['offline_snapshot_positions'] = {
            'fixture_only': True,
            'fixture_market_session': 'NON_TRADING',
            'fixture_trading_day': 20000101,
            'channel_gross_position': channel_total,
            'strategy_position_volume': strategy_total,
            'portfolio_position_volume': portfolio_total,
            'passed': True,
            'boundary': 'Proves saved snapshots can be read without a session gate; does not query CTP or prove snapshot freshness.',
        }

        # Constructing WatchDog loads scheduling records only. Do not run its
        # worker, start an AppInfo process, or create any MQ subscription.
        dog = WatchDog(manager.get_db(), logger=QuietLogger())
        dog.applyAppConf({
            'id': groupid, 'path': sys.executable, 'folder': str(fixture),
            'param': 'CONTROLLED_FIXTURE_NEVER_RUN.py', 'span': 3, 'guard': False,
            'redirect': False, 'mqurl': '',
            'schedule': {
                'active': False, 'weekflag': '0000000',
                'tasks': [{'active': False, 'time': 0, 'action': 0} for _ in range(6)],
            },
        }, isGroup=True)
        stored = manager.get_db().execute('SELECT schedule,guard,mqurl FROM schedules WHERE appid=?', (groupid,)).fetchone()
        restored = WatchDog(manager.get_db(), logger=QuietLogger()).getAppConf(groupid)
        assert stored == ('false', 'false', ''), stored
        assert restored['schedule']['active'] is False
        assert restored['guard'] is False
        report['checks']['disabled_schedule_persistence'] = {
            'input_active': False,
            'sqlite_schedule': stored[0],
            'restored_active': restored['schedule']['active'],
            'guard': restored['guard'],
            'mqurl': stored[2],
            'worker_started': False,
            'passed': True,
        }
        manager.get_db().close()
        report['fixture_directory_was_temporary'] = True
    report['all_passed'] = all(c['passed'] for c in report['checks'].values())
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
