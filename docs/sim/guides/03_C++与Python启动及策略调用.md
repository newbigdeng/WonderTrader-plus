# C++ 与 Python 启动及策略调用

核对日期：2026-10-13。以 VM 当前运行源码及配套 wtpy 为依据；代码是关键摘录，省略部分以文字说明。模拟盘真实验收与架构解释分别表述。

<!-- WT-ILLUSTRATION:02_market_order_chain:START -->
<figure class="wt-illustration">
<a class="wt-image-link" href="../images/02_market_order_chain.png" title="点击打开原图"><img src="../images/02_market_order_chain.png" alt="行情与订单的完整链路" loading="lazy" decoding="async"></a>
<figcaption><strong>行情与订单的完整链路</strong>。先记住：行情MD进中台，目标进执行器，真实委托走交易TD。图中的Python策略区包含同进程的C++引擎与Reader；Bar由数据/引擎层形成，MD直接提供Tick。UDP与SHM是可选传输方式，当前simnow-03用UDP。实际配置与验收状态以正文为准。</figcaption>
<p class="wt-original"><a href="../images/02_market_order_chain.png">打开原图，放大阅读</a></p>
</figure>
<!-- WT-ILLUSTRATION:02_market_order_chain:END -->

## 1．C++ Runner 的启动顺序：配置阶段与连接阶段分开

<!-- WT-ILLUSTRATION:08_cpp_startup:START -->
C++启动分两步：config先加载资料并装配模块，run再连接行情和交易接口、运行引擎。

<figure class="wt-illustration">
<a class="wt-image-link" href="../images/08_cpp_startup.png" title="点击打开原图"><img src="../images/08_cpp_startup.png" alt="C++ Runner启动顺序" loading="lazy" decoding="async"></a>
<figcaption><strong>C++ Runner启动顺序</strong>。模块关系概念图；实际装配和验收结论以正文及源码为准。蓝色表示配置阶段的调用顺序，橙色表示run调用顺序；登录查询完成时间可以交错，引擎不等待所有登录结束才启动。</figcaption>
<p class="wt-original"><a href="../images/08_cpp_startup.png">打开原图，放大阅读</a></p>
</figure>
<!-- WT-ILLUSTRATION:08_cpp_startup:END -->


入口在 `src/WtRunner/main.cpp`：解析命令行，先 `runner.init(logcfg)`，然后 `runner.config(config)`，最后 `runner.run(false)`。`init()` 主要初始化日志、记录主模块目录。

`WtRunner::config()` 的实际顺序是：

1. 读取主 YAML，加载 session→commodity→contract，再加载 holiday、主力/次主力与自定义映射，给合约标记主力属性。
2. `initEngine()`：依据 `env.name` 选择 CTA / HFT / SEL；CTA 进入 `WtCtaEngine::init()`→`WtEngine::init()`。这里读取过滤器和手续费、加载组合检查点、创建输出 CSV、初始化组合风控对象。
3. `initDataMgr()`：`WtDtMgr::init()`→`initStore()`；从 `WtDataStorage` 导出 `createDataReader` 得到 Reader，并把 `data.store` 交给它。
4. `initActionPolicy()`：加载 `bspolicy` 开平规则。
5. `initParsers()`：只处理 `active: true`，加载插件，找 `createParser`，创建行情接口、设置 SPI、准备订阅合约。
6. `initTraders()`：加载交易插件，找 `createTrader`，交给 `TraderAdapter`，读取通道流控与保存记录配置。
7. `initEvtNotifier()`：准备通知组件。
8. `initExecuters()`：加载执行工厂，创建执行器，绑定 TraderAdapter，并通过 `trader->addSink(executer)` 注册订单/成交回报接收者；加载路由。
9. `initCtaStrategies()`：扫描 `cta/` 的 C++ 策略工厂，按 `Factory.Strategy` 创建策略，传入 `params`，创建上下文并交给引擎。

**配置完成之后才进入正式连接与事件循环：**

```cpp
// src/WtRunner/WtRunner.cpp，VM 约 593 行，摘自 run()
_parsers.run();
_traders.run();
_engine->run();
```

ParserAdapter 的 `run()` 调接口 `connect()`；TraderAdapter 的 `run()` 注册 SPI，调 `connect()`、`login()`；CTA 引擎创建并启动 `WtCtaRtTicker`，输出 `marker.json`，启动组合风控线程。

这三个 `run()` 先后执行，不等于“行情登录完再等交易登录，再启动策略”：柜台登录和查询通过异步回调推进。执行器只有交易通道 ready 才允许提交订单。看到“模块加载成功”只证明库和对象准备好了，不能代替认证/登录/初始化查询验收。

当前 Runner 在部分模块初始化失败时仍可能继续注册 Adapter，并存在把交易接口 `init()` 失败误报为 `deleteTrader not found` 的日志。这是当前实现的错误诊断边界，不能把该文字机械解释为导出函数缺失；本次未改这部分逻辑。


## 2．Python 策略仍在使用 C++ 交易内核

<!-- WT-ILLUSTRATION:09_wtpy_startup:START -->
Python用WtEngine启动，通过WtWrapper加载WtPorter；行情、执行和交易仍由C++内核处理，回调再进入Python策略。

<figure class="wt-illustration">
<a class="wt-image-link" href="../images/09_wtpy_startup.png" title="点击打开原图"><img src="../images/09_wtpy_startup.png" alt="wtpy启动与双向桥接" loading="lazy" decoding="async"></a>
<figcaption><strong>wtpy启动与双向桥接</strong>。模块关系概念图；实际装配和验收结论以正文及源码为准。这是显式提交配置的推荐顺序；没有另起WtRunner进程。Python策略用add_cta_strategy注册，执行器工厂从WtPorter安装目录加载；真实柜台订单/成交回报仍由交易与执行层处理。</figcaption>
<p class="wt-original"><a href="../images/09_wtpy_startup.png">打开原图，放大阅读</a></p>
</figure>
<!-- WT-ILLUSTRATION:09_wtpy_startup:END -->


wtpy 并没有再写一套 Python 撮合、行情存储或执行系统。`run.py` 是启动入口，Python 保留策略对象；`WtWrapper` 使用 ctypes 加载 WtPorter，通过 C 导出接口配置 C++ 引擎，再以 C 回调把行情和计算事件送回 Python。

<!-- WT-TEXT-DIAGRAM:FOLD-START -->
<details class="wt-text-diagram" markdown="1">
<summary>展开文字版调用链或目录树</summary>

```text
python run.py
  → WtEngine(ET_CTA)
  → WtWrapper.__init__ / ctypes.cdll.LoadLibrary
  → WtWrapper.initialize_cta：注册事件、策略回调，init_porter
  → WtRtRunner::init：日志、库安装目录、generated 目录
  → WtEngine.init：读取 config.yaml，构建 Python 的品种/合约/时段管理器
  → WtEngine.commitConfig：把最终配置转成 JSON 字符串
  → config_porter(JSON, false)
  → WtRtRunner::config：装配 C++ 引擎、数据读取、开平规则、parser、trader、executer
  → WtEngine.add_cta_strategy：创建 C++ ExpCtaContext，登记 Python CtaContext
  → WtEngine.run / run_porter
  → WtRtRunner::run：parsers.run → traders.run → engine.run
```

</details>
<!-- WT-TEXT-DIAGRAM:FOLD-END -->


`commitConfig()` 只提交一次，`run()` 会在尚未提交时自动调用。实际项目建议显式提交后再添加 Python 策略，使流程容易检查：

```python
from wtpy import WtEngine, EngineType
from MyStrategy import MyStrategy
import time

engine = WtEngine(EngineType.ET_CTA,
                  logCfg="logcfg.yaml", genDir="generated")
engine.init("../common/", "config.yaml")
engine.commitConfig()
engine.add_cta_strategy(MyStrategy(name="rb_short"))
engine.run(True)
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    engine.release()
```

这是入口示例，不代表目前 WtPorter 的退出已经全部验收。当前本地 `src/WtPorter/WtRtRunner.cpp:1190`，以及 VM 同文件的 `release()` 都只调用 `WTSLogger::stop()`，没有显式停止 parser、trader、engine。此前对 C++ WtRunner 的退出修复不能直接当作 Python 入口也已修复；Python 版登录后退出和带仓重启要单独验证。

Linux 库路径是 `wtpy/wrapper/linux/libWtPorter.so`，Windows 64 位是 `wtpy/wrapper/x64/WtPorter.dll`。路径来自 `PlatformHelper.getModule("WtPorter")`，不是由策略配置指定。`WtEngine` 使用 singleton 装饰器，且 `init_porter` 也有一次初始化保护；不同组合应使用不同进程。


## 3．行情如何驱动 Python 策略，信号如何落成订单

行情链路与 C++ 版共用模块：

<!-- WT-TEXT-DIAGRAM:FOLD-START -->
<details class="wt-text-diagram" markdown="1">
<summary>展开文字版调用链或目录树</summary>

```text
CTP MD → QuoteFactory/ParserCTP → DataWriter（实时 mmap、历史落盘）
                                 └→ UDP 广播
                                      → Python 进程中的 ParserUDP
                                      → ParserAdapter → WtCtaEngine
                                      → ExpCtaContext::on_tick_updated / on_calculate
                                      → WtRtRunner::ctx_on_tick / ctx_on_calc
                                      → ctypes 注册的 WtWrapper.on_stra_tick / on_stra_calc
                                      → CtaContext → MyStrategy.on_tick / on_calculate
```

</details>
<!-- WT-TEXT-DIAGRAM:FOLD-END -->


bar 闭合和主 K 线调度由 C++ 数据/引擎层负责。`on_init()` 中调用 `stra_prepare_bars(code,"m1",count,isMain=True)` 标明主 K 线；主 K 线闭合触发 `on_calculate`。`stra_sub_ticks` 订阅额外 tick，`stra_get_bars` 返回 `WtNpKline`，可使用 `.closes/.highs/.lows/.opens/.bartimes`。获取历史 bars/ticks 会自动订阅相应行情。

核心桥接源码很短：

```cpp
// src/WtPorter/ExpCtaContext.cpp
void ExpCtaContext::on_calculate(uint32_t curDate, uint32_t curTime)
{
    getRunner().ctx_on_calc(_context_id, curDate, curTime, ET_CTA);
}
```

```python
# wtpy/wrapper/WtWrapper.py
def on_stra_calc(self, id, curDate, curTime):
    ctx = self._engine.get_context(id)
    if ctx is not None:
        ctx.on_calculate()

# wtpy/CtaContext.py
def on_calculate(self):
    self.__stra_info__.on_calculate(self)
```

提交目标仓位后，Python 即退出这条同步 API 调用，后续真实交易交给原生执行层：

<!-- WT-TEXT-DIAGRAM:FOLD-START -->
<details class="wt-text-diagram" markdown="1">
<summary>展开文字版调用链或目录树</summary>

```text
Python context.stra_set_position(code, qty, usertag)
 → WtWrapper.cta_set_position
 → WtPorter 的 C 导出 cta_set_position
 → ExpCtaContext/CtaStraBaseCtx::stra_set_position
 → 策略理论持仓/条件单/组合汇总
 → 引擎过滤器、组合风控缩放、路由
 → WtLocalExecuter 与执行单元：真实仓位差额、挂单、报价、拆单、超时撤单
 → TraderAdapter：开平规则与交易检查
 → TraderCTP::orderInsert → CTP ReqOrderInsert → SimNow 柜台
```

</details>
<!-- WT-TEXT-DIAGRAM:FOLD-END -->


```cpp
// src/WtPorter/WtPorter.cpp
void cta_set_position(CtxHandler h, const char* code, double qty,
                      const char* tag, double limitpx, double stoppx)
{
    CtaContextPtr ctx = getRunner().getCtaContext(h);
    if (ctx == NULL) return;
    ctx->stra_set_position(code, qty, tag, limitpx, stoppx);
}
```

正数代表目标多仓，负数代表目标空仓，0 为平仓目标；不是“每次调用就买卖 qty 手”，也不代表柜台成交。重复指定同一个目标应由执行器管理差额。CTA 的 `stra_get_position` 是策略上下文持仓，不应当成直接查询柜台返回的全部持仓。

订单回报反向返回：`CTP OnRspOrderInsert/OnRtnOrder/OnRtnTrade → TraderCTP → TraderAdapter → 交易通道状态、持仓/资金、委托/成交 CSV 与通知 → 执行器及执行单元继续处理撤单或剩余目标`。Python CTA 不提供 `on_order/on_trade` 钩子；订单处理不是再调用 Python CTA 策略。Python HFT 的 `BaseHftStrategy`、`WtWrapper.initialize_hft`、`ExpHftContext` 才显式注册委托/成交/通道/持仓回调。监控仍可读取 CTA 的真实交易通道结果。

trader 的 module 选择 `TraderCTP` 就具备向配置的 CTP 环境报单的能力；选择 `TraderMocker` 就在本地交易插件中模拟。名称通常是 `TraderMocker`，不要凭口语写成不存在的 `tradeMocker`。是否真的产生订单还取决于策略信号、路由和执行器是否启用。运行模式名称 `product` 或目录名字 `sim` 本身不会阻止真实柜台下单。

