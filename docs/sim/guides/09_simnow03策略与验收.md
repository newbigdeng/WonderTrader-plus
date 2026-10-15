# simnow-03：Python 策略、配置和本轮结果

更新：2026-10-14。原趋势策略已经在线接收真实 Tick，并在已有分钟线后触发计算；第一套环境的单手开平委托与真实回报由独立验收策略实测通过。**盈利优势仍未得到证明，空历史自动调度和完整 Python 关闭仍有待解决。**

2026-10-13 的离线结果保留在第4节；10月14日的完整测试、失败、恢复与修复见[11 在线链路验收](11_simnow03在线链路验收.md)。

<!-- WT-ILLUSTRATION:01_config_dependencies:START -->
<figure class="wt-illustration">
<a class="wt-image-link" href="../images/01_config_dependencies.png" title="点击打开原图"><img src="../images/01_config_dependencies.png" alt="配置与目录的依赖关系" loading="lazy" decoding="async"></a>
<figcaption><strong>配置与目录的依赖关系</strong>。先记住：中台和Runner共用基础资料与行情目录，各自读取自己的配置；generated保存运行状态。common.incoming经过审核才发布。这些是文件关系，箭头不是模块启动顺序。实际配置与验收状态以正文为准。</figcaption>
<p class="wt-original"><a href="../images/01_config_dependencies.png">打开原图，放大阅读</a></p>
</figure>
<!-- WT-ILLUSTRATION:01_config_dependencies:END -->

## 1. 实际目录与环境

虚拟机目录 `/home/zjh/wt-lab/records/simnow-03`，Python 为 `/home/zjh/anaconda3/bin/python`，实测 3.12.7。已有 Ubuntu18/GCC8 容器用于编译，不为策略再安装一套 Python；Python 可以在 VM 宿主机加载这些 Linux 库。

```text
simnow-03/
  strategy.json                    算法和预算参数
  strategies/TrendPullback.py       Python 策略
  runtime/wtpy/                    专属包，含已匹配的 .so
  common/                          当前 rb2701 基础资料
  common.incoming/                 Loader查询待审核输出，不自动用于运行
  common_historical/               老历史数据诊断的基础资料
  loader/                          查询工具、私密账户配置与映射
  datahub/                         C++ QuoteFactory、CTP 行情配置
  run/                             Python 主程序与交易配置
  FUT_Data/                        本目录自己的行情输出，已接入第一套CTP
  storage/functional,real,historical/  三种明确区分的数据
  backtest/outputs/数据集/策略名/    离线回测结果
  evidence/                        本轮日志与验收结果
```

“使用 wtpy”指策略与运行入口由 Python 写，行情中台和交易执行仍用 C++。运行程序不是 C++ `WtRunner`，而是 `python run_live.py` 加载 `libWtPorter.so`。

## 2. 新目录实际配置关系

| 文件 | 使用者与依赖 |
|---|---|
| `loader/config.ini` | CTPLoader→第一套交易30003；查询输出到common.incoming |
| `datahub/dtcfg.ctp.yaml` | 中台→common、mdparsers.ctp、statemonitor、Writer、广播列表 |
| `datahub/mdparsers.ctp.yaml` | ParserCTP→30013、SDK库、新flow；订阅 rb2701 |
| `run/config.ctp-observe.yaml` | Python运行内核→Reader、UDP、CTP Trader；执行器为空 |
| `run/config.ctp-trade.yaml` | 同上，改引用 executers.trade；strict_sync=false |
| `run/tdparsers.udp.yaml` | ParserUDP→本机订阅13999、接收19003 |
| `run/tdtraders.ctp.yaml` | TraderCTP→30003、认证与通道频率限制 |
| `run/executers.trade.yaml` | exec03绑定simnow，最小冲击执行单元，lots=1 |
| `run/actpolicy.yaml` | 开仓/平今/平昨拆分规则 |
| `run/filters.yaml` | 初始没有外部策略/合约过滤 |
| `strategy.json` | Python算法读参数；预算2万元，不代表向柜台充值2万元 |
| `run/config.offline.yaml` | 无parser/trader/executer；已用于本轮初始化验证 |
| `backtest/configbt.*.yaml` | 各数据集、日期、tick开关、手续费、基础数据路径 |

CTP 密码只在 VM 本地配置中。SDK 使用生产密钥模式，AppID `simnow_client_test`，认证码 16 个 0。组合 SimpleRiskMon 当前关闭，策略自身风险门禁与交易通道流控启用；不能声称已启用所有 WT 风控。

### 第一套环境的明确配置（2026-10-13核对）

当前三处接入都使用你指定的第一套第三组前置：

| 接入者 | 地址 | 作用 |
|---|---|---|
| LoaderRunner / CTPLoader | `tcp://182.254.243.31:30003` | 一次性查询合约元数据 |
| QuoteFactory / ParserCTP | `tcp://182.254.243.31:30013` | 持续接行情，中台存储并广播 |
| Python运行内核 / TraderCTP | `tcp://182.254.243.31:30003` | 登录、查询账户；启用执行器后才具备报单链 |

Broker 为9999，AppID为simnow_client_test，认证码为16个0，SDK沿用已装配的6.7.13生产密钥模式。行情插件不额外走交易AppID认证；账户与密码已在VM三份配置中核对一致，不复制到HTML。

本轮42项配置/路径检查通过：上述地址与认证、UDP13999→19003、观察执行器为空、交易执行器绑定、一手限制、基础资料、Reader/Writer同一存储路径以及必需插件文件存在。**这只是配置核对，未启动Loader/中台/策略，未测试今天网络连通或发送订单。** 旧目录第一套持续行情通过的记录不等于新Python目录在线闭环已经通过。

Loader原来写入`common`，现已备份私密配置并改为`../common.incoming/`；运行仍读`../common/`，避免全量查询覆盖当前rb2701精简资料。`START-HERE.txt`记录启动顺序，脱敏配置与本轮检查报告已归档。

## 3. 这个短线策略怎样做决策

策略只交易 rb2701。一分钟闭合 K 线计算快 EMA6、慢 EMA18：趋势向上时等回踩后重新站上快线并突破前一根高点，开多；反向条件开空。数据不足时等待，不凭空补历史，不把未闭合 K 线当成已知完整走势。

```python
long_signal = (fast[-1] > slow[-1] and close[-2] <= fast[-2]
               and close[-1] > fast[-1] and close[-1] > high[-2])
context.stra_set_position(code, 1, 'trend_pullback')  # 最大理论目标1手
```

这个调用设置 CTA 理论目标，不是柜台成交确认。挂单、撤单、追单、CTP 回报由 C++ 执行器处理；`pending` 只防止理论目标未反映时重复提交，不能当作真实订单超时管理。

| 风险措施 | 实际规则 |
|---|---|
| 单笔数量 | 最大目标±1手，不加仓、不补亏损仓 |
| 单笔预算 | ATR14×1.5，至少4跳；止损估算损失加10元费用不超过预算权益0.5% |
| 保证金估算 | 假设20%，占策略预算不超过35%；不是实际经纪商费率 |
| 实际资金 | 必须有新鲜CNY可用资金快照；超过180秒不新开仓 |
| 止盈/止损 | 盈利达2倍止损距离或亏损达到止损距离，目标归零；跳空与滑点可能越过止损 |
| 时间与频率 | 最多持有12根主K线，每天最多3次开仓，平仓后冷却3根 |
| 日内亏损 | 策略累计日内损益达到-200元，平仓并停止新开仓 |
| 收盘前 | 11:25、14:55、22:55后不留目标仓；没有Tick时不能保证平仓能执行 |

计数、日初损益、止损距离和冷却时间通过 userdata 保存；此版本 **拒绝带柜台持仓启动交易**，不承诺完整带仓恢复。2万元、20%保证金和每手每边2元回测费用都是设计/诊断假设，实际手续费、可用资金和保证金应核对柜台。每跳金额=跳价×合约乘数，本品种乘数10、跳价1与官方螺纹合约规则一致。[上期所业务细则](https://www.shfe.com.cn/regulation/exchangerules/productrules/202512/t20251231_829962.html)

## 4. 2026-10-13 的离线验证结果

| 检查 | 数据与模型 | 实际结果 |
|---|---|---|
| Python运行内核 | WtEngine/WtPorter，所有通道关闭 | on_init真实回调成功，进程返回0；不代表CTP带仓退出通过 |
| 策略功能回放 | 900个人工盘口Tick、225根m1；causal_touch、1跳滑点 | 三次开平、6笔成交，目标最终为空；返回0，功能通过 |
| 真实短行情 | 今日中台240Tick/3根Bar转DSB，causal_touch | 回放成功；尚未达到预热，无开仓，不能检验盈利 |
| 历史诊断 | 2019-09-02至11-29，rb.HOT五分钟Bar；legacy_cta、1跳滑点、每边2元 | 18个平仓回合，毛损益-470、费用72、净损益 **-542元**；最终空仓 |
| 风险门禁 | 新策略的10个有意义场景及1个启动门禁 | 入场1手、止损/止盈、日亏、收盘平仓、次数上限、旧资金/低资金/保证金、观察模式均通过 |

人工场景净收益168元是人为路径下的输出，**不作为收益证明**。历史诊断使用了不同周期和旧连续合约数据，不能代替当前一分钟策略的样本外验证；它仍明确告诉我们，现有参数没有获得盈利认可。因此下一阶段应先观察行情和信号，再决定是否启用一手试验；不能因“程序跑通”就宣传能赚钱。

测试期间发现两项装配/数据问题：旧桥库没有新wrapper要求的v2导出，已针对WtBtPorter重编译；人工初稿刚好没有突破前高，未覆盖开仓分支，已修正人工路径并只重跑该功能场景，保留前次无信号结果。没有搜索历史参数凑正收益。

## 5. 之后开盘怎么启动

2026-10-14 已执行中台、原策略观察与独立单手委托验收；下面是后续运行原趋势策略的操作。当前common已准备好目标品种，首次观察不需要先查一遍Loader。若以后需更新合约，单独执行下面的一次性查询，检查common.incoming中的rb2701、乘数、跳价与session映射后再决定发布，不能整目录覆盖或与交易登录同时运行：

```bash
cd /home/zjh/wt-lab/records/simnow-03/loader
TZ=Asia/Shanghai ./LoaderRunner -c config.ini
```

然后在 VM 启动中台（Python策略仍使用这个C++中台）：

```bash
cd /home/zjh/wt-lab/records/simnow-03/datahub
TZ=Asia/Shanghai ./QuoteFactory -c dtcfg.ctp.yaml -l logcfgdt.yaml
```

首次空数据目录先等中台积累至少20根一分钟线，再启动策略，避免主K线未注册而只有Tick、没有计算回调。本轮已复现该问题，重启消费者后绕过，C++调度尚未修改。缓存上限72根，算法最低需要20根；已有数据时按实际可用数量判断。

另一个终端先运行观察模式，连接CTP查询并从中台接行情，但没有执行器、策略不提交任何目标：

```bash
cd /home/zjh/wt-lab/records/simnow-03/run
/home/zjh/anaconda3/bin/python run_live.py
```

待你明确要求开盘启动交易、在官方终端确认空仓且没有未完订单后，先停止观察进程，进入同一run目录，才使用：

```bash
/home/zjh/anaconda3/bin/python run_live.py --trade --account-checked --state-dir generated_trade
```

交易启动在创建 WtEngine 之前检查状态目录必须全新且为空，拒绝加载旧理论仓位、待信号或组合目标。因为 C++ 恢复这些状态发生在 Python armed 门禁之前，仅靠 armed=False 不够。观察后转交易使用新的 `generated_trade`；若该目录已有内容，程序直接拒绝启动，不自动删除状态。此策略不支持带仓恢复。新目录会丢失旧userdata中的当日计数、日初损益和冷却，因此当前尚不支持跨重启日内风控；同日不应通过反复新建目录重新获得风险额度，重复启动需先人工核对当日累计交易和损益。

程序随后检查新鲜资金快照、可用资金和空持仓。`--account-checked` 是人工完成订单对账的标记，程序没有自动读取官方终端。`strict_sync=false` 避免启动时清掉未知账户持仓，但不能替代对账。

Ctrl+C调用现有 `engine.release()`。使用 `--state-dir` 后监控不会自动跟随该目录，需按07篇的固定generated路径规则另行安排监控部署。当前 WtPorter 源码只显式停止日志，没有证明完全关闭交易、行情和引擎线程；已有无通道初始化的退出0不能覆盖有持仓的退出。所以第一次在线试验应先完成平仓、确认柜台无挂单，再停止并检查；Python带仓退出、重启和故障恢复是待验证项，不在本次准备报告中写成通过。

## 6. 怎样复跑本轮离线验证

```bash
cd /home/zjh/wt-lab/records/simnow-03/backtest
/home/zjh/anaconda3/bin/python run_bt.py --dataset functional
/home/zjh/anaconda3/bin/python run_bt.py --dataset real
/home/zjh/anaconda3/bin/python run_bt.py --dataset historical
/home/zjh/anaconda3/bin/python check_risk.py
```

输出在 `backtest/outputs/functional|real|historical/trend_pullback/`，汇总、日志在 `evidence/`。三种数据的用途不同，不把人工场景、真实短记录和历史诊断合成一份“盈利回测”。

<details markdown="1">
<summary>展开完整 Python 策略与交易启动门禁源码</summary>

### strategies/TrendPullback.py

```python
"""One-lot intraday example. Research candidate, not a profit promise."""
import math
import time
from datetime import datetime
import numpy as np
from wtpy import BaseCtaStrategy


class TrendPullback(BaseCtaStrategy):
    def __init__(self, name, code='SHFE.rb.2701', capital=20000,
                 fast=6, slow=18, atr_period=14, risk_fraction=0.005,
                 daily_loss=200, max_entries=3, max_hold_bars=12,
                 margin_rate=0.20, margin_cap=0.35, armed=True, period='m1'):
        super().__init__(name)
        assert 1 < fast < slow and capital > 0
        self.code, self.capital = code, float(capital)
        self.fast, self.slow, self.atr_period = fast, slow, atr_period
        self.period = period
        self.risk_fraction, self.daily_loss = risk_fraction, daily_loss
        self.max_entries, self.max_hold_bars = max_entries, max_hold_bars
        self.margin_rate, self.margin_cap = margin_rate, margin_cap
        self.count = max(slow * 4, atr_period + 2)
        self.tick_size, self.multiplier = 1.0, 10.0
        self.stop_distance, self.entry_price = 0.0, 0.0
        self.bar_no, self.entry_bar, self.last_flat_bar = 0, 0, -100
        self.day, self.entries, self.day_start_pnl = 0, 0, 0.0
        self.last_pos, self.pending = 0, None
        self.pending_bar, self.halted = 0, False
        self.last_bar_time = 0
        self.events = []
        self.calculations = 0
        self.armed, self.broker_available = armed, None
        self.broker_updated_at = 0.0
        self.last_flat_stamp = 0

    @staticmethod
    def ema(values, period):
        result = np.empty(len(values)); result[0] = values[0]
        alpha = 2.0 / (period + 1)
        for i in range(1, len(values)):
            result[i] = alpha * values[i] + (1 - alpha) * result[i - 1]
        return result

    def on_init(self, context):
        info = context.stra_get_comminfo(self.code)
        self.tick_size, self.multiplier = float(info.pricetick), float(info.volscale)
        context.stra_prepare_bars(self.code, self.period, self.count, isMain=True)
        context.stra_sub_ticks(self.code)
        self.day = int(context.user_load_data('risk_day', 0, int))
        self.entries = int(context.user_load_data('entries', 0, int))
        self.day_start_pnl = float(context.user_load_data('day_start_pnl', 0.0))
        self.stop_distance = float(context.user_load_data('stop_distance', 0.0))
        self.last_flat_stamp = int(context.user_load_data('last_flat_stamp', 0, int))
        context.stra_log_text(f'TrendPullback: one lot, closed {self.period} bars, stop/target/time exit')

    def sync_day(self, context):
        day = context.stra_get_tdate()
        if day != self.day:
            self.day, self.entries = day, 0
            self.day_start_pnl = context.stra_get_fund_data(0)
            context.user_save_data('risk_day', day)
            context.user_save_data('entries', 0)
            context.user_save_data('day_start_pnl', self.day_start_pnl)

    @staticmethod
    def may_enter(time):
        return (905 <= time < 1010 or 1040 <= time < 1120 or
                1335 <= time < 1450 or 2105 <= time < 2250)

    @staticmethod
    def must_flat(time):
        return 1125 <= time < 1300 or 1455 <= time < 2000 or time >= 2255 or time < 800

    def target(self, context, qty, reason):
        if not self.armed:
            return
        if self.pending == qty:
            return
        context.stra_set_position(self.code, qty, reason)
        self.pending, self.pending_bar = qty, self.bar_no
        self.events.append({'date':context.stra_get_date(), 'time':context.stra_get_time(),
                            'target':qty, 'reason':reason})
        context.stra_log_text(f'target={qty}: {reason}')
        if qty:
            self.entries += 1
            context.user_save_data('entries', self.entries)

    def position(self, context):
        pos = context.stra_get_position(self.code)
        if abs(pos) > 1:
            self.halted = True
            context.stra_log_text('Unexpected strategy position >1: halt, manual reconciliation required', 3)
        if self.pending is not None and pos == self.pending:
            self.pending = None
        if pos and not self.last_pos:
            self.entry_bar = self.bar_no
            self.entry_price = context.stra_get_position_avgpx(self.code)
        if not pos and self.last_pos:
            self.last_flat_bar = self.bar_no
            self.last_flat_stamp = int(datetime.strptime(f'{context.stra_get_date():08d}{context.stra_get_time():04d}', '%Y%m%d%H%M').timestamp()//60)
            context.user_save_data('last_flat_stamp', self.last_flat_stamp)
        self.last_pos = pos
        return pos

    def check_exit(self, context, price):
        self.sync_day(context)
        pos = self.position(context)
        if not pos or self.halted or self.pending == 0:
            return
        if context.stra_get_fund_data(0) - self.day_start_pnl <= -self.daily_loss:
            self.target(context, 0, 'daily_loss'); return
        if self.must_flat(context.stra_get_time()):
            self.target(context, 0, 'session_flat'); return
        cost = context.stra_get_position_avgpx(self.code) or self.entry_price
        if cost <= 0 or self.stop_distance <= 0:
            self.target(context, 0, 'missing_recovery_stop'); return
        signed_profit = (price - cost) * (1 if pos > 0 else -1)
        if signed_profit <= -self.stop_distance:
            self.target(context, 0, 'stop_loss')
        elif signed_profit >= 2 * self.stop_distance:
            self.target(context, 0, 'take_profit')
        elif self.bar_no - self.entry_bar >= self.max_hold_bars:
            self.target(context, 0, 'time_exit')

    def on_tick(self, context, stdCode, newTick):
        if stdCode == self.code and newTick.get('price', 0) > 0:
            self.check_exit(context, float(newTick['price']))

    def on_calculate(self, context):
        bars = context.stra_get_bars(self.code, self.period, self.count, isMain=True)
        if bars is None or len(bars.closes) < max(self.slow + 2, self.atr_period + 2):
            return
        stamp = int(bars.bartimes[-1])
        if stamp == self.last_bar_time:
            return
        self.last_bar_time = stamp; self.bar_no += 1; self.calculations += 1
        price = float(bars.closes[-1]); self.check_exit(context, price)
        pos = self.position(context)
        if self.pending is not None:
            if self.bar_no - self.pending_bar >= 3:
                self.halted = True
                context.stra_log_text('Target not reflected in strategy position: halted', 3)
            return
        if not self.armed or self.halted or pos or not self.may_enter(context.stra_get_time()):
            return
        if self.entries >= self.max_entries or self.bar_no - self.last_flat_bar < 3:
            return
        now_minute = int(datetime.strptime(f'{context.stra_get_date():08d}{context.stra_get_time():04d}', '%Y%m%d%H%M').timestamp()//60)
        if self.last_flat_stamp and now_minute-self.last_flat_stamp < 3*int(self.period[1:]):
            return
        if context.stra_get_fund_data(0) - self.day_start_pnl <= -self.daily_loss:
            return
        close = np.asarray(bars.closes, dtype=float)
        high, low = np.asarray(bars.highs, dtype=float), np.asarray(bars.lows, dtype=float)
        ef, es = self.ema(close, self.fast), self.ema(close, self.slow)
        tr = np.maximum(high[1:] - low[1:], np.maximum(abs(high[1:] - close[:-1]), abs(low[1:] - close[:-1])))
        atr = float(np.mean(tr[-self.atr_period:]))
        if not math.isfinite(atr) or atr <= 0:
            return
        stop = max(4 * self.tick_size, math.ceil(1.5 * atr / self.tick_size) * self.tick_size)
        equity = self.capital + context.stra_get_fund_data(0)
        # CTA fund data is strategy P&L, not broker available margin.
        risk = stop * self.multiplier + 10  # conservative example round-trip fee allowance
        estimated_margin = price * self.multiplier * self.margin_rate
        if equity <= 0 or risk > equity * self.risk_fraction or estimated_margin > equity * self.margin_cap:
            return
        if not context.is_backtest:
            if (self.broker_available is None or not math.isfinite(self.broker_available) or
                    self.broker_available<0 or time.time()-self.broker_updated_at>180 or
                    estimated_margin+100>self.broker_available):
                return
        long_signal = ef[-1] > es[-1] and close[-2] <= ef[-2] and close[-1] > ef[-1] and close[-1] > high[-2]
        short_signal = ef[-1] < es[-1] and close[-2] >= ef[-2] and close[-1] < ef[-1] and close[-1] < low[-2]
        if long_signal or short_signal:
            self.stop_distance = stop
            context.user_save_data('stop_distance', stop)
            self.target(context, 1 if long_signal else -1, 'trend_pullback')

```

### run/run_live.py

```python
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
```

</details>


2026-10-14 启动门禁修正：平仓后 `positions` 可能保留零数量记录；现在按多空今仓/昨仓数量判断，并拒绝负数和非有限数。8个针对性场景通过。该程序仍要求人工核对所有未完成委托，`--account-checked` 不是自动对账。正式趋势策略没有在本轮启用自然信号下单，闭环验收结果见[11](11_simnow03在线链路验收.md)。
