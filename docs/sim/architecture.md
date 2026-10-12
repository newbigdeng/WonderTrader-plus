# 模拟盘主要链路与状态职责

## 两条行情消费支路

```mermaid
flowchart TD
    Sender[人工 Tick 发送器] -->|UDP 9101| Hub[QuoteFactory / ParserUDP]
    Hub --> Data[ParserAdapter / DataManager]
    Data --> Store[WtDataStorage 缓存与存储]
    Store --> Caster[UDPCaster]
    Caster -->|UDP 9001| Runner[WtRunner / ParserUDP]
    Runner --> CTA[WtCtaTicker / WtCtaEngine / CTA 策略]
    CTA --> Exec[WtLocalExecuter / WtMinImpactExeUnit]
    Exec --> Adapter[TraderAdapter]
    Adapter -->|委托| Mocker[TraderMocker]
    Caster -->|UDP 9002| Mocker
    Mocker -->|订单与成交回报| Adapter
    Adapter -->|执行反馈| Exec
```

发送器只向行情中台入口发数据。本次没有直接向 9001/9002 发送而绕过中台。Runner 与 TraderMocker 各有独立行情 socket 和缓存，Runner 收到行情并不保证模拟撮合器已经拿到同一报价。

## 各层负责什么

| 模块 | 状态与职责 |
| --- | --- |
| QuoteFactory / ParserAdapter | 行情接入、合约识别，交给中台 |
| WtDataStorage | Tick 缓存、实时存储块、分钟线与输入校验 |
| UDPCaster | 异步广播给下游消费者 |
| WtCtaTicker / WtCtaEngine | 时间推进、策略分发、目标汇总 |
| CtaStraBaseCtx | 策略理论持仓、理论成交及组合资金记账 |
| WtLocalExecuter / WtMinImpactExeUnit | 比较目标、通道持仓和在途委托，决定报单 |
| TraderAdapter | 接收交易回报，维护通道订单、持仓与账户状态 |
| TraderMocker | 独立报价缓存、在途订单、本地撮合与模拟通道持仓 |

策略理论成交价不会被后来的通道实际成交价自动替换。因此理论持仓/资金与交易通道持仓/流水需要分别核对。

相关源码：[`WtCtaTicker.cpp`](../../src/WtCore/WtCtaTicker.cpp)、[`CtaStraBaseCtx.cpp`](../../src/WtCore/CtaStraBaseCtx.cpp)、[`TraderAdapter.cpp`](../../src/WtCore/TraderAdapter.cpp)、[`TraderMocker.cpp`](../../src/TraderMocker/TraderMocker.cpp)、[`WtDataWriter.cpp`](../../src/WtDataStorage/WtDataWriter.cpp)。

## 线程与可重复性

策略和执行器线程池设为 0，仍存在 UDP I/O、异步 writer、广播、CTA 时钟和模拟撮合线程。本次观察 Runner 为 6 个线程，中台输入前为 7、输入后为 9，不能把线程池关闭理解为整个进程单线程。

人工输入的价格路径可以重复，订单遇到的报价仍受异步调度影响。固定拆量为 1 手消除了该部分数量随机性，没有建立下单完成后再推进下一 Tick 的事件屏障。

当前只验证低速输入。异步队列与 UDP 广播的高负载积压、丢包和背压边界尚未测量。

## 合约代码与输入连续性

实验原始 Tick 使用交易所 `SHFE`、合约 `rb2301`；订阅原始完整名为 `SHFE.rb2301`，CTA 标准代码与执行路由为 `SHFE.rb.2301`。这些字符串属于不同接口层。

恢复后既要加载持仓，也要续接同交易日的累计行情量。缓存已有 300 时从 10 重新发送，会触发累计量回退校验；这与持仓是否加载成功是两项独立检查。

## 2026-10-02 正常关闭与恢复

停止外部行情输入，等时钟和引擎工作结束，再保存策略最终状态；交易接口停止撮合和 I/O 后保存通道实际持仓。两份账本保留各自职责，部分成交时目标可以与实际持仓不同。

线程局部池入口退出不再直接使仍被其他线程引用的对象失效：共享池状态跟随存活对象一起保留。相关模块必须统一重新编译。

重启保持既有策略/执行器/通道 ID 和状态目录，按目标与实际持仓差额重新执行。模拟器没有恢复旧在途订单的能力；真实 CTP 接入要先查询柜台订单与持仓。见 [本轮修复记录](repair-20261002.md) 与 [SimNow 教程](simnow-guide.md)。
