# simnow-03：真实行情、模拟委托与恢复验收

更新：2026-10-14。**第一套环境的行情→Python→执行器→CTP→真实模拟成交回报已经跑通，最后账户空仓，没有活动委托。** 但不能说“全部都没问题”：空历史启动的主 K 线调度、启动脚本的持仓判断和验收脚本的 CSV 编码都在本轮暴露了问题。原趋势策略只完成在线观察；用于主动下单的是另写的单手链路验收策略，盈利与长期运行仍未验证。

## 1．这次究竟测了什么

使用 VM `/home/zjh/wt-lab/records/simnow-03`，Python 3.12.7、专属 wtpy/WtPorter、此前隔离适配的 CTP `v6.7.13_20260225`。行情接第一套 `30013`，交易接 `30003`，只交易 `SHFE.rb.2701`。数据中台从 21:26:52 运行到约 22:00:54；账户交易日为 **20261015**，Tick 自然日为 **20261014**，这是夜盘日期的正常区别。

| 验收层 | 实际证据 | 结论 |
|---|---|---|
| CTP 行情接入 | 中台登录、订阅 rb2701；连续写入 4,065 条 Tick CSV | 通过本次窗口 |
| 中台存储 | `cache.dmb`、Tick/min1/min5 DMB 和当日 Tick CSV 均生成 | 实时写入通过；尚未验收收盘转存历史文件 |
| UDP→Python | 原趋势策略两轮分别收到 360、299 条实时 Tick，盘口和日期字段有效 | 接收通过；未测压力下丢包率 |
| 分钟调度 | 第二轮读取 23～25 根 K 线，触发 3 次 `on_calculate` | 有分钟数据后通过；空数据启动存在问题 |
| 交易准备 | 官方 SDK 查询全账户持仓、订单、CNY 资金；初始空仓、无活动订单 | 通过；不是仅检查 WT 的已知标签订单 |
| Python 目标→真实报单 | 验收策略设置 +1/0 目标；原执行器发送开多和平今委托 | 两轮开平通过，共 4 笔委托 |
| 回报与实际账 | 每笔都有柜台订单号、成交号，`volume=1`、`traded=1` | 委托、成交、实际持仓联动通过 |
| 带仓重启 | 首轮脚本失败后恢复同一状态，读回 1 手持仓，完成平今，无重复开仓 | 该单一场景通过 |
| 最终核查与停止 | 官方 SDK 再查持仓 0、活动订单 0；本次启动的所有进程已退出 | 清理完成；完整原生关闭流程仍待审查 |

本轮没有重新运行 Loader：已有 rb2701 基础资料足够，不需要每次启动都全量查询或覆盖 `common`。也没有运行全量回归、监控网页或实盘账户。

## 2．行情怎样真正送到策略

```text
SimNow Market Front 30013
  → ParserCTP / QuoteFactory
  → WtDataStorage：实时 DMB、mmap 文件、Tick CSV
  → UDP：127.0.0.1:19003
  → ParserUDP / Python 进程里的 WtPorter
  → WtCtaEngine / CtaStraBaseCtx
  → Python TrendPullback.on_tick

分钟线另由同一 FUT_Data 的 Reader 读取、更新
  → 主 K 线闭合调度
  → Python TrendPullback.on_calculate
```

Runner 的行情 parser 是 UDP，**本轮没有让 Python 运行内核再直连 CTP 行情**。TraderCTP 连交易前置负责查询、报单与回报。订单没有经过 UDP 行情广播。

Python 实际收到的是扁平字段：`action_date`、`action_time`、`trading_date`、`price`、`bid_price_0`、`ask_price_0`、`bid_qty_0`、`ask_qty_0`。第一版观察脚本误用数组字段，写出的空值是观察脚本取错键；原策略读取 `price` 的逻辑没有受影响。

中台的 Tick CSV **没有表头**，4,065 行就是 4,065 条记录。每行携带交易日和自然日；不能用 CSV 第一行充当标题，也不能仅凭文件存在判定策略已经接到行情。本轮另外记录了 Python 回调计数来核验消费端。

## 3．订单与回报的完整闭环

为了不依赖趋势信号何时出现，使用独立 `one_lot_acceptance` 策略。它先核对全账户空仓、无活动订单、资金足够和实时盘口，再只发一次 1 手开多目标；确认柜台成交流水后，设置目标 0。使用原交易配置、开平规则、通道风控和 `WtMinImpactExeUnit`，没有用 SDK 工具绕过执行器直接下单。

```text
Python on_tick → context.stra_set_position(code, 1/0)
  → C ABI / ExpCtaContext / CtaStraBaseCtx
  → WtCtaEngine 的目标仓位与组合净额
  → WtLocalExecuter / WtMinImpactExeUnit
  → TraderAdapter：开平拆分、自成交检查、通道频率限制
  → TraderCTP::orderInsert → CTP ReqOrderInsert

CTP OnRtnOrder / OnRtnTrade
  → TraderCTP 转为 WT 订单/成交对象
  → TraderAdapter::onPushOrder / onPushTrade
  → 更新实际持仓、未完成量，通知执行器
  → traders/simnow/orders.csv、trades.csv、rtdata.json
```

CTA 目标与策略输出是理论账。普通 CTA Python 策略不直接接收逐笔柜台成交回调；真实回报由交易适配器和执行器处理。因此本轮用 **traders 目录的成交流水＋独立柜台查询**确认成交，未把 `stra_get_position()==1` 当作已成交证明。

| 场景 | 开多 1 手 | 平今 1 手 | 模拟净损益 |
|---|---|---|---:|
| 首轮开仓后恢复平仓 | 21:53:44，3060，订单 80602 | 21:55:27，3059，订单 82176 | -16.129 元 |
| 修正后的连续闭环 | 21:58:39，3059，订单 84465 | 21:58:45，3058，订单 84511 | -16.127 元 |

两轮毛损益各为 `(平仓价－开仓价)×10×1 = -10` 元。柜台合计手续费 **12.256 元**，净损益合计 **-32.256 元**。这是立即开平的链路验收成本，不能评价趋势回踩算法是否有优势。

日志里的未完成量 `0→-1→0` 表示一手卖单的有符号净数量，和以前错误的“累计成交量为负数”不同。本次订单累计成交量均为正 1，日期均非 0。订单 CSV 的自然日 20261014 与账户交易日 20261015 也不冲突。

## 4．发现的问题、来源和处理

### A．空历史启动：Tick 有了，分钟计算没有自动开始

第一轮几乎同时启动中台与原策略：180 秒收到 360 条 Tick，却没有一次 `on_calculate`。第二轮在中台已经积累分钟线后启动，150 秒收到 299 条 Tick，并有 3 次计算回调。

VM `CtaStraBaseCtx::stra_get_bars` 只有在取到 K 线切片时才建立 `_kline_tags`；`on_schedule` 又在遍历这个集合时调用策略计算。集合为空时，循环里的“空集合也计算”条件根本到不了。下面是核心逻辑的缩略摘录：

```cpp
WTSKlineSlice* kline = _engine->get_kline_slice(...);
if (kline) {
    KlineTag& tag = _kline_tags[key];
    tag._closed = false;
}

for (auto it = _kline_tags.begin(); it != _kline_tags.end(); it++) {
    // 集合为空，循环体不会执行。
    if (isMainUdt || _kline_tags.empty())
        on_calculate(curDate, curTime);
}
```

这个机制与本轮现象吻合，但第一轮没有保存到有效原生日志，**不能把第一次取线返回值说成已捕获的断点证据**。当前状态是“现象已复现，源码支持该定位，启动顺序已绕过”；本轮没有修改 C++ 调度源码。

当前操作方法：先启动中台，确认行情持续进入，让它积累至少 20 根一分钟线，再启动原趋势策略；首次空目录通常需要约 20 分钟有效行情。策略请求缓存上限为 72 根，但入场计算的最低数量是 20 根。后续应做明确的“数据就绪→主 K 线注册”重试，不能依赖没有开始的 `on_calculate` 自己预热。

`on_bar` 为 0 不作为本轮失败：原策略没有显式订阅 `stra_sub_bar_events`，它用主 K 线的 `on_calculate`。

### B．零数量持仓记录误判：修复 simnow-03 启动程序

第一轮平仓后，WT 的快照仍可能保留该合约的一行记录，但多空 `newvol/prevol` 都是 0。第二次验收没有发单，被原来的判断拦住。同样的判断也存在于 `run_live.py`，因此正常平仓后再次启动会被错误拒绝。

```python
# 原判断：列表非空就当作持仓。
if state.get('positions'):
    raise RuntimeError('Broker positions are not empty')

# 修正：检查实际多空数量，包含今仓和昨仓。
from account_checks import position_volume
if position_volume(state) > 0:
    raise RuntimeError('Broker positions are not empty')
```

`position_volume` 对每条记录的多、空今仓及昨仓求和；数量必须有限且非负。不能只求多空净差，否则多空各 1 手会被误认为没有风险。空列表、零行、今多、昨空、双向各一手、负数、NaN、无穷共 **8 个针对性场景通过**；随后真实闭环复测通过。异常结构直接拒绝，不当作空仓。

这是 **simnow-03 Python 启动门禁的语义修复**，没有重编译 WT 内核。实际 VM 文件已更新，原稿保存在证据目录。原策略的入场信号、参数和日内风控规则没有修改。

### C．柜台 CSV 编码：修正验收脚本，保留首次失败

首次开仓已成交，我的脚本用 UTF-8 读 `orders.csv`，遇到 CTP 中文备注而解码失败，脚本提前退出，留下本次 1 手测试持仓。这一错误没有导致柜台拒单或丢成交。

<details class="source" markdown="1">
<summary>展开验收脚本CSV读取修改前后代码</summary>

修改前的实际失败行如下。它使用当前Python环境默认的UTF-8；原始行保留在首次失败的`roundtrip/stdout.log`堆栈中，不声称完整旧脚本另有快照。

```python
with p.open() as f:return list(csv.DictReader(f))
```

修正后的实际函数保留在同轮证据的`run/online_roundtrip.py`：

```python
def rows(name):
    p=S/'traders/simnow'/name
    if not p.exists():return []
    raw=p.read_bytes()
    try:text=raw.decode('utf-8')
    except UnicodeDecodeError:text=raw.decode('gb18030')
    return list(csv.DictReader(text.splitlines()))
```

这里的`S`是该次验收状态目录，`name`为`orders.csv`或`trades.csv`。先严格尝试UTF-8，失败才严格尝试GB18030；两种都失败会继续报错，不忽略字节，也不把解码错误当作“没有订单”。这只是10月14日验收脚本的修正，不是WT统一字符编码改造，本次补充对照没有重新运行交易。

</details>

`TraderCTP` 原样保存 `StatusMsg`；已有 wtpy 监控读取订单 CSV 使用 GB2312。本轮改为严格尝试 UTF-8，失败后严格尝试兼容该字节集的 GB18030，没有忽略字节或把异常当作空文件。恢复原状态目录、原策略 ID 后，只关闭经原始账户查询确认的本次 rb2701 多仓；平仓后核查全账户，未新增开仓。

该问题属于 **验收脚本读文件的假设错误**。首次失败日志、带仓快照、恢复日志、后续成功结果均保留；未宣称修复了 WT 的统一字符编码设计。

### D．日志配置路径：按当前接口要求传相对路径

观察脚本前两轮传了绝对日志配置路径；当前 `WtRtRunner::init` 使用 `cwd + logCfg`，无法正确读取该路径，因此没有原生运行日志。委托测试改为传入相对于 `run` 的配置路径，日志正常生成。只修正了测试入口的用法，没有修改 C++ 路径处理。

## 5．当前可以放心说什么，还有什么不能说

本轮已证明：指定第一套环境、rb2701、当前 SDK 与运行包下，持续行情、UDP 到 Python、已有分钟线后的策略计算、单手开平、CTP 委托/成交回报以及最后空仓核查可以联动。恢复实验还证明了 **这一笔自有多仓、同一状态目录、无悬挂订单**可以被恢复并平掉。

仍需验证：原趋势策略自然触发后的真实交易、做空方向、部分成交与撤改单、断网重连、跨交易日、异常数据、压力与多策略共享账户。空历史自动调度尚未修复；`run_live.py` 的 `--account-checked` 仍是操作者声明，不能替代自动全账户未完成订单查询。新状态目录会重置部分策略日内状态，不能称为跨重启账户级风控。

Python `engine.release()` 当前原生实现主要停止日志，并非完整关闭所有交易、行情、ticker 线程。本轮各消费进程最终已退出，中台收到 SIGINT 后停止；**进程已经结束，不等于 Python 完整资源关闭全部验收通过**。首轮失败后没有重复开仓，也不等于所有崩溃恢复场景已经覆盖。

结束后未保留任何交易进程。下次启动仍需核对账户与活动订单，继续使用原趋势策略入口，不把验收策略当成日常交易算法。

## 6．源码与证据入口

关键源码函数：`CtaContext.stra_set_position`、`WtWrapper.cta_set_position`、`CtaStraBaseCtx::stra_get_bars/on_schedule`、`WtLocalExecuter::set_position`、`WtMinImpactExeUnit::do_calc/on_order/on_trade`、`TraderAdapter::onPushOrder/onPushTrade`、`TraderCTP::orderInsert/OnRtnOrder/OnRtnTrade`。按函数名定位，Windows 学习注释版与 VM 行号可能不同。

- [结果汇总](../evidence/simnow03-online/summary.json)：行情数量、文件、费用、最终状态。
- [零数量持仓门禁场景](../evidence/simnow03-online/account-check-cases.json)。
- [修正后的启动程序](../evidence/simnow03-online/run_live.py)与[数量检查函数](../evidence/simnow03-online/account_checks.py)。
- [官方 SDK 查询器](../evidence/simnow03-online/ctp_account_audit.cpp)：查询全账户，不含报单调用，不写入密码。
- [成功闭环记录](../evidence/simnow03-online/roundtrip3-status.json)与[最终柜台核查](../evidence/simnow03-online/final-account.json)。
- 原始失败、恢复、后续成功日志与 CSV 阅读副本集中在个人目录 `_evidence/simnow03/online-20261014`；VM 原始证据位于 `simnow-03/evidence/online-20261014`，私密 API 流文件只留在 VM。

回到[09 策略与启动教程](09_simnow03策略与验收.md)或[阅读目录](README.md)。
