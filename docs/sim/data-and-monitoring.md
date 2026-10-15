# 数据落盘与监控：先分清读到的是哪本账

实时控制台 WtMonSvr 读取运行中的文件和事件；WtBtSnooper 查看回测结果；回测进度由 MQ 事件观察。原生 C++ Runner 也能被监控，不要求改成 Python 策略。

2026-10-05 对 wtpy 0.9.9.3 副本做源码核对和隔离读盘，复现两处显示错误。完整网页、MQ 与在线任务尚未验收，监控源码也尚未修复。

## 1. 数据在哪里，监控读取哪一份

程序运行时，行情缓存、策略信号、理论持仓、执行器在途单和通道持仓主要在 C++ 内存中。文件是这些状态的流水或快照，不是每个对象的实时镜像。

下面以 **Runner 的工作目录**为起点。若从其他目录启动，相对路径的落点也会改变；配置文件所在位置不自动等于工作目录。

| 数据 | 默认位置 | 内容与写入时机 |
| --- | --- | --- |
| 策略清单 | generated/marker.json | 引擎登记策略、通道和执行器；控制台据此发现对象 |
| CTA 理论流水 | generated/outputs/策略名/ | trades、closes、signals 等 CSV 在相应事件追加；funds、positions 主要在会话结束写结算记录 |
| 策略恢复状态 | generated/stradata/策略名.json | 持仓明细、资金、待消费信号、条件单等；仓位变化、调度、会话结束及正常关闭等触发整体保存 |
| 组合理论数据 | generated/portfolio/ | 引擎汇总的 trades/closes/funds.csv 和 datas.json；不是柜台账户总资产 |
| 交易通道流水 | generated/traders/通道ID/ | trades.csv 在成交回报时追加；orders.csv 在订单终态时追加，需 savedata=true |
| 通道快照 | generated/traders/通道ID/rtdata.json | TraderAdapter 在资金查询应答等路径保存通道持仓和分币种资金 |
| 本地模拟器恢复文件 | 如 mocker_1/positions.json | TraderMocker 自己保存实际模拟持仓；它与上述 rtdata.json 分工不同 |
| 回测输出 | outputs_bt/策略名/ | 默认输出根目录，可由 WtBtEngine(outDir=...) 改变 |

行情原始数据另由 QuoteFactory 的 writer.path 指定，例如 FUT_Data。WtDataWriter 更新缓存和实时 .dmb 映射块，历史转换写 .dsb；这些二进制行情文件与交易 CSV 分开。监控中的 K 线由 WtDtServo 读取行情存储，不是从 trades.csv 生成。

之前模拟实验在 Linux VM/容器内运行。个人 sim/_evidence 中的是下载后的证据副本，适合复核历史；指向它只能看历史快照，不能监控容器正在运行的策略。

### 两本账必须分别看

策略目标三手、通道只成交一手时，策略理论记录与实际通道记录可以不同。控制台的“策略”页读 CTA 理论账本，“通道”页读接口成交和持仓；判断是否真实重复下单，应核对通道原始流水、orderid/tradeid 和柜台查询。

本地 TraderMocker 的账户资金查询返回零，不代表流水毛盈亏为零。SimNow/实盘则以柜台账户查询为准；CTA 理论 dynbalance 通常是累计损益口径，不能直接当作含初始本金、保证金和出入金的账户余额。

## 2. CSV、JSON 和回测输出是怎样写的

实时 CTA 的 CtaStraBaseCtx::init_outputs 创建文件，新文件写表头，已有文件定位到末尾。log_signal、log_trade、log_close 拼装 CSV 行，经 BoostFile::write_file 写入。理论成交源于策略上下文的仓位处理；不是 TraderAdapter 收到柜台成交后回填这份价格。

TraderAdapter::onPushTrade 更新通道状态，调用 logTrade 写实际成交行，再请求账户查询；onRspAccount 调用 saveData 生成 rtdata.json。订单 CSV 则在 onPushOrder 中满足 **!orderInfo->isAlive()** 才写。因此一张三手单仅成交一手时，trades.csv 可以已有一行、orders.csv 仍没有终态行，这不等于没有委托。

JSON 用 RapidJSON 构造对象并序列化，通过 create_new_file 覆盖旧快照。CSV 更像追加流水，JSON 更像最新一次保存的状态。源码中的普通文件写入不等于 fsync 或跨文件原子提交；并发读取恰好遇到覆盖过程可能读到空或不完整 JSON，控制台部分读取路径会捕获异常后返回空数据。**页面空白不能直接解释成账户空仓。**

运行中策略快照仍存在此前 SIM-04 的保存顺序问题；正常退出补最终保存改善了最终状态，没有让每次中间快照都与内存同步。详见 [修复记录](repair-20261002.md)。

回测 CTA 的写法不同：CtaMocker::log_trade 等方法先追加到内存 stringstream；handle_replay_done 调用 dump_outputs，把表头和全部缓冲内容一次写入 CSV，再输出状态、图表等数据。默认路径由 WtBtCore::WtHelper 管理，wtpy 经 WtBtEngine → WtBtWrapper → WtBtPorter 把 outDir 传给 C++。因此策略一般不需要自己写结果 CSV。

persistData=false 会关闭该策略的部分结果持久化；bNeedDump 主要控制回放状态等输出，不能把这两个开关当成同一个。普通回测未结束时可能只有旧文件、元数据或没有成交流水；异步运行也不会自动变成逐笔刷盘。


## 3. 监控链路怎样接

```text
实时 C++ → generated 下的 CSV/JSON → DataMgr → HTTP 查询
实时/回测事件 → EventNotifier → WtMsgQue → EventReceiver → 网页推送
行情文件 → WtDtServo → K 线查询
```

监控应部署在能读实时运行目录的环境。组合 path 指向含 generated/marker.json 的 Runner 工作目录；下载到 Windows 的实验副本只能看历史。

先确认监控读取的流水与原 CSV 一致，再接 MQ。notifier 与订阅端地址必须相同；它使用 nanomsg PUB/SUB，与行情 UDP 广播是两条链。PUB/SUB 没有历史补发，晚订阅可能错过事件。

回测结束后先确认 trades、closes、funds 和 btenv，再由 Analyst 生成摘要，Snooper 的工作空间指向输出根目录。K 线数据由 DtServo 读取行情存储，不能把回测 CSV 目录当行情存储。

回测过程中用 BT_STATE 看进度、BT_FUND 看会话资金。btenv 只在有限位置写入，不能当连续进度。在线任务管理会创建自己的任务与子进程，并不会自动接管任意已启动的 runBT.py。

具体启动、账户初始化和完整观察器脚本保留在个人离线操作稿。先通过文件、事件和状态一致性验收，再启用页面进程控制与守护重启。

## 4. 发现问题后怎样判断

| 编号 | 结论与状态 | 使用时的处理 |
| --- | --- | --- |
| MON-01 通道记录重复 | **已复现、已定位，未修复。** 真实 CSV 各 2 行，三次读盘返回 2/4/6；get_channel_orders/get_channel_trades 追加缓存后不推进 lastrow | 不依据页面条数判定重复成交；核对原 CSV 与订单/成交 ID |
| MON-02 回测手续费显示错误 | **已复现、已定位，未修复。** 构造 price=3000、fee=2 的原生格式行，Snooper 返回 fee=3000；正确读取 cells[7] 后又被 cells[4] 覆盖 | 单笔费用和相关展示先查原 CSV；未证明所有统计页面都受影响 |
| MON-03 在线模板不匹配 | **源码已定位，未端到端验证。** WtBtMon 要 configbt.json、logcfgbt.json、fees.json，随附 template 实为 YAML 且缺这些 JSON 文件 | 先整理模板与 runBT.py 配置引用，不能仅取消示例注释就认为可运行 |
| MON-04 在线资金事件未处理 | **已定位为实现边界。** WtBtMon::on_fund 为 pass | 用观察器接收原始 BT_FUND；网页动态曲线待实现/验收 |
| MON-05 简易分析摘要路径 | **源码已定位，未执行该方法。** run_simple 用 folder + 'summary.json'，缺目录分隔符 | 本教程选路径拼接正确的 run_new；其完整统计仍需真实回测验收 |

验证实际做了：隔离调用 DataMgr，读取 10 月 2 日回归归档中的基线输出；调用 Snooper 的信息、资金、成交读盘方法，使用原生 CtaMocker 格式的构造文件；核对自定义用户写入后重新加载的登录摘要契约。10 个读盘调用完成，并复现上述两项语义错误，**这不是监控整体验收通过**。

没有做：依赖安装、完整服务启动、浏览器交互、真实 MQ 推送、WtDtServo 历史 K 线、在线回测任务、监控启停/守护恢复、SimNow 或实盘运行。没有修改 wtpy 监控源码。读盘隔离验证使用 AST 提取原有类，避开未安装依赖，不等价于完整 wtpy 导入成功。

还要注意读盘实现大量使用 split(',') 而非完整 CSV 解析。自定义 tag/remark 不宜包含逗号、换行；编码不一致、读到半行、JSON 瞬间不完整或文件被替换，都可能导致异常或显示遗漏。监控应和原始文件、程序日志及柜台查询相互校验。


## 5. 对应源码

WT 的 CtaStraBaseCtx 负责实时策略流水和快照，TraderAdapter 负责通道回报与查询状态，CtaMocker 在回测结束导出内存日志。wtpy 的 DataMgr 管用户/组合元数据与读盘缓存，Snooper 读回测结果，WtDtServo 提供行情查询。

排查页面异常时先核对这些职责和原文件，再检查缓存及消息推送。页面展示成功本身不构成交易对账通过。
