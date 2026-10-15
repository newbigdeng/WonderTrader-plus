# wtpy 自带监控：部署与实际操作

更新：2026-10-15，下午补充日志开关与源码对照。**原生监控已运行，MQ空闲后不再发送事件的源码问题已修复：真实CTP观察日志能实时滚动；受控原生委托、撤单和成交事件能到达浏览器，撤单与成交提示已在界面看到。** 普通委托不弹提示、表格仍靠刷新，这是本版前端的既有设计。

上午MQ修复和下午日志开关测试都没有新增CTP订单。上午11:46的柜台核对为空仓、无活动委托；**“新增CTP开平→监控提示”尚未在开盘时补验**。当前继续保留原趋势策略的观察模式，执行器为空；昨天的真实模拟成交放在两个独立档案中。原因与测试边界见第6节，全部修复的修改前后代码放在第9节按需展开；监控各页面的机制详解见[14号](14_监控原理与页面读写链路.md)。

## 1．现在怎样打开

Windows 浏览器打开：**[wtpy 原生监控](http://127.0.0.1:8099/console/index.html)**。

账号为 `localadmin`。密码见[本机登录资料](F:/0_MyLog/WonderTrader_Log/EveryDay/sim/_evidence/simnow03/monitor-20261015/login.local.json)，这是新建的监控账号，和 SimNow 期货账号不同。资料只留在 VM 和个人本机副本，没有写入仓库。

已经替你打开并登录了页面；另开浏览器时，输入上述用户名和资料中的 `password`，点击“登录”。建议使用宽一些的浏览器窗口，原生桌面布局较宽；列显示不全时可横向滚动表格。

服务在 VM 的 `127.0.0.1:8099`，Windows 已建立 SSH 本机转发。地址能打开的前提是 VM、监控服务和转发进程仍在运行；它没有直接监听公网地址。

## 2．先选对左边的组合

| 组合名称 | 用来查看什么 | 当前状态 |
|---|---|---|
| **simnow-03 实时观察** | 原 TrendPullback、当前柜台持仓与资金、当前运行日志 | 中台与 Python 正在运行；不启用下单 |
| **验收档案① 开仓后恢复平仓** | 10月14日首轮开仓、脚本失败后恢复平仓 | 历史快照；2笔委托、2笔成交；不配置启动程序 |
| **验收档案② 连续单手开平** | 10月14日修正后的连续开平 | 历史快照；2笔委托、2笔成交；不配置启动程序 |
| **原生MQ受控验收（不连接柜台）** | 本次原生事件链测试，合约显示为`TEST.MQ.0001` | 测试已结束；只有通知器，没有行情、交易接口和执行器；不是CTP成交档案 |

点击的是**组合名称**，不是旁边的“启动/停止”按钮。原生页面可能默认选中档案①，需要手动点击实时组合才能看当前状态。两个档案是原始运行状态的复制快照，原实验目录没有改写；归档不是一份正在交易的策略。

现在实时组合空仓、没有本轮新增成交，所以相关表格为空是正常结果。查看昨天真实的模拟成交应选择档案，不能把档案中的记录当成刚刚发生的新交易。

## 3．五类数据的点击方法

切换组合或页签后，**表格先点击右侧数据区自己的“刷新”**；需要持续重读文件时，再勾选该表的“自动刷新”。日志区现在也有独立的**“自动刷新”复选框和“刷新”按钮**，实时、历史模式都能自己选。它们不会启用策略下单、停止程序或改变其他表格的刷新选择。

| 想看什么 | 操作路径 | 怎样理解 |
|---|---|---|
| 当前策略 | 实时组合→**策略管理**→策略下拉框选择 `trend_pullback` | 这是 Python 策略 ID，来自 `generated/marker.json` |
| 策略理论持仓 | 策略管理→**持仓明细**→刷新 | 策略认为自己持有的数量，不是柜台成交确认 |
| 柜台实际持仓 | **通道管理→持仓明细**→选 `simnow` 或全部通道→刷新 | 来自交易通道快照；当前没有非零持仓 |
| 实际委托 | 选择档案→**通道管理→订单明细**→选 `simnow`→刷新 | 看委托单号、数量、累计成交量、撤销标志 |
| 实际成交 | 选择档案→**通道管理→成交明细**→选 `simnow`→刷新 | 看成交单号、关联委托单号、价格和数量 |
| 当前账户资金 | 实时组合→**通道管理→资金明细**→刷新 | 看币种CNY、保证金、手续费、可用资金等 |
| 理论表现 | 策略管理→**每日绩效**，或组合管理 | 理论策略/组合数据；部分日线报表要到日终才产生 |
| 是否仍在计算 | 实时组合→中间**滚动日志** | 新日志直接追加；开盘有行情时可看到分钟调度，休市时可能暂时没有新日志 |

### 日志怎样暂停、恢复与补看

| 当前组合 | 勾选“自动刷新” | 取消勾选 | 点击“刷新” |
|---|---|---|---|
| 实时组合，datmod=auto | 随MQ追加新日志 | 暂停当前浏览器日志追加 | 读Runner日志文件最后100行 |
| 历史/手动组合，datmod=mannual | 每15秒读一次日志文件 | 停止日志轮询 | 立即读最后100行 |

实时组合恢复勾选后接着显示**后续**消息，不会依靠MQ补回暂停期间的历史；想补看时点击“刷新”。这次受控原生测试依次发送A、B、C：A正常显示，暂停后B不追加，恢复后C显示；再次暂停并手动刷新，能从文件补看到B。测试组合没有柜台接口，验收结束后已移除临时登记。

选择按“这个浏览器访问地址＋组合ID”保存在localStorage。切回同一个组合仍记得选择，实时组默认开、历史组默认关；不会写进SQLite，也不保证换浏览器或换本机端口后沿用。浏览器禁用存储时开关仍可使用，只是不保留偏好。

暂停只控制日志区域：行情、策略、柜台通道、磁盘日志和订单/成交通知继续按原方式运行。资金和持仓表格的自动刷新是另外的复选框。

### 先用档案②核对一遍

在“通道管理→订单明细”刷新后，应看到 **2 笔**，每笔数量1、成交1、未撤销：

| 委托单号 | 动作 | 委托时间 | 价格 |
|---|---|---|---:|
| 84465 | 开多 | 2026/10/14 21:58:39 | 3059 |
| 84511 | 平今多 | 2026/10/14 21:58:45 | 3058 |

再切到“成交明细”刷新，应看到对应成交号 **60703、60733**。这两条来自昨天的真实 SimNow 模拟成交文件，不是本轮填造的演示数据。档案①另有首轮的两条记录，合计与11号报告的4笔一致。

### 再看当前资金

实时组合的资金页已经实际显示：

| 界面列 | 显示值 |
|---|---:|
| 通道 / 币种 | simnow / CNY |
| 上日结存 | 20,000,000.00 |
| 静态权益（当前字段映射） | 19,999,967.74 |
| 平仓盈亏 | -20.00 |
| 浮动盈亏 / 保证金 | 0.00 / 0.00 |
| 手续费 | 12.26 |
| 可用资金 | 19,999,967.74 |

网页保留两位小数，原始快照的可用资金为 **19,999,967.744**、手续费为 **12.256**，四舍五入不是丢账。通道资金是整个账户的快照，包含此前交易日内累计结果，不是只属于当前选中策略的资金。

## 4．最容易看错的三件事

**策略账和柜台账不同。** “策略管理”的成交、信号、回合与收益用于解释理论策略运行；“通道管理”才是柜台回报与实际持仓、资金。档案左侧显示的 `-4.00` 是理论组合统计，不是账户余额，也不是两轮实际净损益。实际两轮净损益合计 **-32.256元**，核算见11号报告。

**刷新网页不等于向 CTP 重新查询。** 网页主要重读 `generated` 中的 JSON/CSV。TraderCTP 继续运行、向柜台查询并写出 `rtdata.json`，资金才会更新。12号独立查询工具的 `account_latest.json` 不会自动接入这个监控。表格底部时间是网页刷新时间，也不能单独证明柜台快照刚刚更新。

**空委托表不等于柜台没有挂单。** 本版 `TraderAdapter` 主要在订单结束时把记录写入 `orders.csv`；重新启动的观察进程也不会自动把全部历史柜台成交补写进新CSV。本次已验证委托事件传输，但原生界面没有因此变成完整的活动委托列表；完整核对仍看原始柜台查询。

## 5．这次是怎么装配的

```text
simnow-03/
  monitor/
    .venv/                    Web依赖；共享已有numpy/pandas
    run_monitor.py            启动原生WtMonSvr，监听VM回环8099
    start_monitor.sh          前台启动入口；进程锁防重复
    bootstrap.py              创建本机账号、登记3个组合和观察入口
    data.db                   用户、组合路径、进程管理设置
    login.local.json          私密登录资料
    logs/WtMonSvr.log         监控自己的服务日志
    views/acceptance01/       档案①索引、generated快照副本、日志入口
    views/acceptance02/       档案②索引、generated快照副本、日志入口
    runtime-fixes.patch       本轮隔离运行包的两项小修复
    requirements-monitor.lock 已验证的Web依赖版本
  run/
    run.py                    监控管理的入口，固定进入观察模式
    run_live.py               原有观察/交易程序；本轮未传--trade
    config.yaml               链接到config.ctp-observe.yaml，供原生配置树识别
    generated/                本次真实观察输出，原生监控可直接找到
  runtime/wtpy/               仍使用本目录的专属wtpy和原生库
```

原生前端会固定寻找组合路径下的 `run.py`、`config.yaml/config.json`、`generated` 和 `Logs`。因此补了观察入口与配置文件链接；归档目录另外放置明确标注 `archive_only` 的索引配置，满足文件树接口，**不是伪造当时的交易配置**。

数据路径是：`SQLite里的groups.path → 组合/generated/marker.json → 策略与通道ID → JSON/CSV`。当前实时组合指向 `simnow-03/run`；两个档案指向 `monitor/views` 下各自的目录。SQLite不保存成交账本，也不会把网页用户恢复成柜台持仓。

MQ 地址仍是 `ipc:///home/zjh/wt-lab/records/simnow-03/run/monitor.ipc`。WatchDog 已识别现有观察进程并建立订阅；**进程守护与定时调度均关闭**。服务日志中的“自动调度服务已启动”只表示看门狗线程启动，不表示某个组合已开启自动拉起。

## 6．MQ为什么没通过，怎样修复，测试到了哪里

### 6.1 首次部署的结果继续保留

首次部署修复了隔离wtpy副本中的以下问题，当时尚未重编C++。原记录及55秒未收到原生日志的失败证据都保留，本次是在此基础上继续定位。

| 项目 | 原因、处理和结果 |
|---|---|
| 刷新后委托/成交重复 | DataMgr读新增行时没有推进`lastrow`；原档案连续读取是2、4、6条。补游标更新后保持2、2、2，网页也显示2笔 |
| 关闭调度却保存为开启 | WatchDog把整个schedule字典当布尔值；改为读`schedule["active"]`。数据库实查guard=false、schedule=false |
| 新账号明文API能登录、网页却失败 | 官方前端先MD5口令，后端再组合loginid计算摘要。初始化账号需按现有前端协议传摘要；已修正bootstrap，网页登录成功，未改变用户输入的口令 |
| 归档切换时报配置树错误 | 原生接口要求config文件存在；补归档索引与日志入口，相关HTTP接口通过 |

首次的网页登录、三个组合的数据读取与重复刷新等32项只读检查通过，WebSocket订阅和心跳也通过，但55秒内没有LOG帧；当时没有测试新订单的推送。**心跳由WebSocket端点直接应答，不经过原生通知器，所以心跳成功不能证明MQ工作。**

### 6.2 真正阻断发布的是C++工作对象寿命

链路是：`EventNotifier → libWtMsgQue的IPC发布 → EventReceiver → WatchDog/AppInfo → WtMonSvr → PushServer → WebSocket → 原生网页`。LOG、TRD_ORDER、TRD_TRADE通过同一个原生异步队列。

原`init()`把Asio的`work`放在初始化线程的局部作用域里，函数返回就销毁。队列空闲后，`run_one()`退出并使服务进入停止状态，后续`post()`不会自行重启；外层线程仍在循环，却不处理新通知。因此磁盘日志和交易通道可以继续工作，网页旁路通知却断了。

修复让`work`覆盖工作线程的寿命，使用`run()`等待任务；析构先`stop()`唤醒线程，再`join()`，最后销毁MQ发布端。顺带明确初始化动态函数指针，并给异步委托通知补上与`retain()`配对的`release()`。**完整旧/新源码见第9节A—D。** 本次确认通知器自己的空闲唤醒和退出，不代表整个wtpy交易进程的全部退出路径已验收。

### 6.3 配置、热改地址和前端开关分别处理

上次实时组合填`mannual`，前端按原设计忽略MQ日志。上午先改为`auto`使实时消息可见；下午再按本次要求加入独立复选框，让数据来源和用户是否继续刷新分开控制。这两步不能混成一个配置修复。

`AppInfo.applyConf()`运行中换MQ地址时，原来重建接收器漏传`sink=self`，清空地址又留着已释放引用。现补齐接收者，并把释放后的引用设为None。独立回调测试旧版转发0条且重复释放，修复版转发2条、每个旧接收器只释放一次。这是确实存在的独立缺陷，**不是固定MQ地址最初断流的直接触发点**；旧/新代码见第9节E。

本轮还把先前只修在隔离包中的通道CSV游标、schedule.active修复同步到wtpy主源码，历史两笔记录连续读取仍为2、2、2，关闭调度落库和重新载入均为false；代码见F—G。账号初始化、datmod分别是调用方式与展示配置纠正，见H—I；前端开关和异步回读保护见K。

原生通知器修复已同步到WT源码，并在上午定向编译部署simnow-03的WtPorter。下午日志开关只更新静态前端，不重启监控或交易程序，没有更换CTP SDK及交易适配库。

### 6.4 上午MQ修复的实际验收结果

| 验证内容 | 实际结果 | 能证明什么 |
|---|---|---|
| 修复前后真实C++源码对照 | 空闲后发送2条LOG、2条ORDER、1条TRADE：旧版收到0条；修复版5条全收到 | 使用实际EventNotifier和WtMsgQue，复现并修复空闲断流 |
| 委托对象及析构 | 对照对象引用计数由旧版2变为修复版1；修复版完成`NOTIFIER_DESTROYED`并以0退出 | 已处理的委托通知不再漏release，空闲通知器能退出 |
| 真实CTP观察进程 | 11:46重启无执行器的观察进程，收到26条真实原生日志；网页追加登录、持仓更新、`Trading channel ready` | 真实运行日志经过MQ到达网页；没有点击日志刷新 |
| 三轮受控原生事件 | 3条日志、6条委托、3条成交，12条全部在WebSocket记录中出现，无重复 | 原生对象序列化、IPC接收、监控转发均通过 |
| 页面显示 | 自动刷新未勾选；出现`MQ_UI_PUSH_BURST_0`、撤单990002和成交990003提示 | 展示来自推送，而非HTTP轮询文件 |
| 上午11:46柜台只读对账 | 总持仓0、活动/不确定委托0；仍为此前4笔已完成委托；余额与可用资金19,999,967.744，保证金0 | 本轮未增加真实柜台订单或账户风险敞口 |

受控程序构造真正的`WTSOrderInfo/WTSTradeInfo`，调用原生`EventNotifier::notify`，走正式的C++发布与监控接收路径，**没有直接给网页伪造WebSocket包**。但它不包含Parser、Trader或Executer，`mq_probe / TEST.MQ.0001`是测试标记，**不代表CTP受理或成交**，也没有往真实账户的CSV里填造数据。测试进程已经退出。

12条事件在同一VM上，从原生序列化时间到WebSocket采集器收到，样本延迟约**537～570毫秒，中位数555毫秒**。这只是这次样本；原PushServer还有约1秒的批处理等待，不能把它当作交易延迟、网页绘制延迟或性能保证。上午MQ修复未改PushServer或前端；下午另行增加日志开关，没有修改交易语义。

页面核验时还观察到部分日志、提示重复展示；独立采集器的原生测试帧和26条真实日志帧均未重复，前端展示重复的来源本轮未定位。**界面提示次数不能作为成交笔数**，应按通道、订单号和柜台/CSV核对。这项展示问题与本次已经复现、修复的原生停止发布问题分开记录。



### 6.5 为什么普通委托仍不弹窗，表格也不立即增加

前端`processEvent`对order有`if (!data.canceled) return`：**普通委托回报能到浏览器，但原版只提醒撤单；trade回报会提示成交**。本次普通委托990001的WebSocket帧已核对，撤单990002、成交990003的提示已在实际网页核验。这是显示策略，不能再把“没有普通委托弹窗”判成MQ丢消息。

委托/成交表格仍通过HTTP读CSV，事件通知没有自动把每个回报插入表格。MQ也不是历史账本，监控后连或断线时不能依赖它补齐所有旧回报；查看历史仍需文件，活动挂单核对仍需柜台。

**尚未通过的是修复后“新增第一套CTP订单→真实成交→监控提示”的整段在线验收。** 修复完成时已过上午交易时段，本轮没有在休市期留下测试挂单。已有证据覆盖真实CTP日志和原生回报到网页的受控传输，不能把两段拼写成一次真实新增开平已经成功。

## 7．以后重启与停止的方法

当前已启动，无需重复运行。VM重启后可在一个终端启动监控：

```bash
cd /home/zjh/wt-lab/records/simnow-03/monitor
./start_monitor.sh
```

在Windows PowerShell新开终端建立转发，保持该终端打开：

```powershell
ssh -N -o ExitOnForwardFailure=yes -L 127.0.0.1:8099:127.0.0.1:8099 zjh@192.168.29.128
```

若本机8099已有当前转发，不要再建一份。VM本机浏览器也可直接访问同一个回环URL。网页登录资料与组合设置保存在 `monitor/data.db`，重启监控不会清除它们。

监控本身不产生行情与资金。需要继续实时观察时，另两个终端依次运行：

```bash
# 中台终端
cd /home/zjh/wt-lab/records/simnow-03/datahub
TZ=Asia/Shanghai ./QuoteFactory -c dtcfg.ctp.yaml -l logcfgdt.yaml

# 观察终端
cd /home/zjh/wt-lab/records/simnow-03/run
/home/zjh/anaconda3/bin/python run.py
```

已有观察进程时不要再运行第二份。当前观察入口没有下单参数；以后正式交易应按09篇先对账、安排状态目录，并同步调整监控映射，不能为了让监控找到文件就删除旧持仓状态或绕过新目录门禁。

前台启动的监控用Ctrl+C停止。现有后台监控可核对 `monitor/monitor.pid` 后向该监控进程发送SIGTERM；停止监控不会停止中台与观察进程。**组合的“停止”按钮是进程控制，不是平仓按钮**：本版Linux WatchDog使用kill -9。因此本次没有点击它验收带仓停止；将来交易运行时应先完成平仓和挂单核对，再安排退出，完整Python关闭仍有待处理。

## 8．数据为空或打不开时先查哪里

| 现象 | 先做什么 |
|---|---|
| Windows网址打不开 | 确认VM已开、SSH转发在、本机端口未冲突；再看monitor.stdout.log |
| 登录失败 | 输入监控localadmin及本机资料中的密码；不要填SimNow账号，也不要把MD5摘要当作网页登录密码 |
| 有组合但没有策略 | 检查该组合的generated/marker.json与groups.path；不能只登记父目录simnow-03 |
| 切换页签仍显示旧表或空表 | 点击当前数据区的刷新，核对当前组合、策略/通道选择和底部时间 |
| 实时组没有成交 | 当前是观察模式；昨天的成交在两个验收档案里 |
| 每日绩效或曲线空 | 日终CSV可能还没有产生；当前账户资金应看通道管理的资金明细 |
| 行情暂停 | 区分市场休市、数据中台状态和网页服务状态；网页仍可显示最后一份快照 |

本篇以真实登录、接口、原生进程、WebSocket记录和浏览器DOM核验为依据。截图另存受到浏览器安全策略限制，未将概念图冒充实际界面。首次部署见[原始验收](../evidence/monitor/verification.json)；本次见[分层验收](../evidence/monitor/mq-fix/verification.json)、[原生对照](../evidence/monitor/mq-fix/native-regression.json)、[热改回调对照](../evidence/monitor/mq-fix/hot-reconfigure-regression.json)、[C++补丁](../tools/monitor/native-mq/native.patch)和[WatchDog补丁](../tools/monitor/native-mq/watchdog-mq.patch)。[原生测试程序](../tools/monitor/native-mq/native_mq_probe.cpp)没有柜台接口。

实际源码基点为VM WT `d816d8c5`、wtpy `1c7d3d3`，叠加已有本地修改及本次修复；本次Git范围是原生通知器与wtpy监控相关修改；其他历史本地模拟修复的代码对照不代表所有旧工作区改动一并合入。完整文本证据、原文件和修复前文档归档到个人`_evidence/simnow03/monitor-20261015/mq-fix`。旧库基线仍保留在VM的`monitor/evidence/mq-fix-20261015/libWtPorter.before.so`。定向构建的旧打包步骤缺少ParserShm副本，最终使用现有CMake的原链接命令成功生成WtPorter，没有复制或覆盖旧CTP模块；构建日志如实保留。登录资料、数据库和会话文件没有复制到仓库。

完整页面机制见[14 监控原理](14_监控原理与页面读写链路.md)，历史本地模拟修复代码见[15 修复对照](../repair-20261002.md)。基础解释见[07 监控与SQLite](07_wtpy监控与SQLite.md)，交易实测见[11 在线验收](11_simnow03在线链路验收.md)，独立资金查询见[12 账户查询工具](12_SimNow账户资金查询工具.md)。

## 9．监控修复的修改前后源码

本节核对了留存的旧源文件、实际补丁与当前源码。片段只保留能说明问题的部分，省略处不表示完整可替换函数。**程序缺陷、初始化方式错误、展示配置纠正分别标注**，不能把改一个配置值写成修改了交易引擎。

修复范围应按阶段理解：2026-10-15首次部署先在simnow-03隔离包修正缓存与调度读取；随后修复、构建原生通知器，并将MQ热改地址修复同步到wtpy主源码；下午再增加用户可选日志刷新，并同步此前的游标、调度修复到主源码。源码和已部署结果应按各自日期与测试范围理解。

### A．原生通知器：空闲后不再发布事件

**源码修复。** 文件：`src/WtCore/EventNotifier.cpp`，函数：`EventNotifier::init`。旧文件为个人证据中的 `mq-fix/EventNotifier.before.cpp`，补丁为 `docs/sim/tools/monitor/native-mq/native.patch`。

<details class="source" markdown="1">
<summary>展开修改前、修改后代码与原因</summary>

修改前：`work`在初始化线程里创建，`init`返回时销毁；工作线程空闲后，后续`post`无法自行恢复停止的事件循环。

```cpp
if (_worker == NULL)
{
    boost::asio::io_service::work work(_asyncio);
    _worker.reset(new StdThread([this]() {
        while (!_stopped)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            _asyncio.run_one();
        }
    }));
}
```

修改后：把`work`放进工作线程，使其覆盖事件循环的整个工作期；`run`自行等待任务，不再每2毫秒轮询。

```cpp
if (_worker == NULL)
{
    _worker.reset(new StdThread([this]() {
        boost::asio::io_service::work work(_asyncio);
        _asyncio.run();
    }));
}
```

LOG、TRD_ORDER、TRD_TRADE共用这条队列。因此坏的是监控通知发布，不能据此断言策略计算、报单或磁盘日志也停止。

</details>

### B．原生通知器：先唤醒退出，再等待线程

**与A配套的生命周期修复。** 同文件，函数：`EventNotifier::~EventNotifier`。

<details class="source" markdown="1">
<summary>展开修改前、修改后代码与退出边界</summary>

修改前：先设置标志并`join`，之后才`stop`事件循环。

```cpp
EventNotifier::~EventNotifier()
{
    _stopped = true;
    if (_worker)
        _worker->join();

    _asyncio.stop();

    if (_remover && _mq_sid != 0)
        _remover(_mq_sid);
}
```

修改后：先`stop`解除Asio的阻塞等待，再`join`，最后销毁MQ发布端。

```cpp
EventNotifier::~EventNotifier()
{
    _asyncio.stop();
    if (_worker)
        _worker->join();

    if (_remover && _mq_sid != 0)
        _remover(_mq_sid);
}
```

A中改成长期有效的`work`后，只写`_stopped=true`不能唤醒正在`run()`里等待的线程，因此析构顺序必须一起改。此次测试证明空闲通知器能够结束；`io_service::stop()`并不承诺排空所有尚未执行的通知，这也不能代替完整交易进程的有序关闭验收。

</details>

### C．原生通知器：初始化所有动态函数指针

**防止未初始化成员被读取的源码修复。** 同文件，函数：`EventNotifier::EventNotifier`。

<details class="source" markdown="1">
<summary>展开修改前、修改后构造函数</summary>

修改前：只有发布指针被明确置空；创建、销毁、日志注册指针没有初值。

```cpp
EventNotifier::EventNotifier()
    : _mq_sid(0)
    , _publisher(NULL)
    , _stopped(false)
{
}
```

修改后：全部明确置空。

```cpp
EventNotifier::EventNotifier()
    : _mq_sid(0)
    , _creator(NULL)
    , _remover(NULL)
    , _publisher(NULL)
    , _register(NULL)
    , _stopped(false)
{
}
```

这是初始化失败、未启用等路径上的对象状态保护，不是已经证明“所有动态库缺符号情形都处理完整”。当前`init`仍需与完整动态库契约配套；不能把补初值夸大成兼容任何版本插件。

</details>

### D．原生委托通知：补齐保留引用的释放

**源码修复。** 同文件，`EventNotifier::notify`的委托重载。异步任务需要保留订单对象，任务完成后也必须释放自己的引用。

<details class="source" markdown="1">
<summary>展开修改前、修改后代码</summary>

修改前：入队前`retain()`，发布后没有与之配对的`release()`。

```cpp
ordInfo->retain();
_asyncio.post([this, strTrader, strCode, localid, ordInfo]() {
    std::string data;
    orderToJson(strTrader.c_str(), localid, strCode.c_str(), ordInfo, data);
    if (_publisher)
        _publisher(_mq_sid, "TRD_ORDER", data.c_str(), (unsigned long)data.size());
});
```

修改后：发布后释放通知任务所持的引用。

```cpp
ordInfo->retain();
_asyncio.post([this, strTrader, strCode, localid, ordInfo]() {
    std::string data;
    orderToJson(strTrader.c_str(), localid, strCode.c_str(), ordInfo, data);
    if (_publisher)
        _publisher(_mq_sid, "TRD_ORDER", data.c_str(), (unsigned long)data.size());
    ordInfo->release();
});
```

同输入对照里，发布完成后的对象引用计数从旧版2恢复为新版1。这里验证的是已经执行完成的通知；关闭时尚未执行任务的清理不在这一条断言内。

</details>

### E．运行中更换MQ地址：接收器要接回上层

**独立复现的Python源码修复。** 文件：`wtpy/monitor/WatchDog.py`，函数：`AppInfo.applyConf`。它不是首次固定地址断流的直接触发点。

<details class="source" markdown="1">
<summary>展开修改前、修改后代码</summary>

修改前：重建接收器时没传`sink`；清空地址时，成员仍指向已经释放的接收器。

```python
if self._mq_url != old_mqurl:
    if self._evt_receiver is not None:
        self._evt_receiver.release()

    if self._mq_url != '':
        self._evt_receiver = EventReceiver(
            url=self._mq_url, logger=self.__logger__)
        self._evt_receiver.run()
```

修改后：先释放并清空旧对象，再把新接收器的事件接回本`AppInfo`。

```python
if self._mq_url != old_mqurl:
    if self._evt_receiver is not None:
        self._evt_receiver.release()
        self._evt_receiver = None

    if self._mq_url != '':
        self._evt_receiver = EventReceiver(
            url=self._mq_url, logger=self.__logger__, sink=self)
        self._evt_receiver.run()
```

定向对照为旧版转发0条、修复后2条；每个旧接收器只释放一次，清空地址后成员为`None`。完整补丁保留了原来的日志语句，这里只省略与缺陷无关的那一行。

</details>

### F．委托和成交列表重复：读完文件要推进游标

**Python源码修复。** 文件：`wtpy/monitor/DataMgr.py`，函数：`get_channel_orders`和`get_channel_trades`。两处都从`lastrow`之后读取，但旧代码没有更新游标。

<details class="source" markdown="1">
<summary>展开两处旧/新代码与2→4→6的原因</summary>

委托列表修改前，省略逐列解析部分：

```python
last_row = trdCache["lastrow"]
lines = f.readlines()
f.close()
lines = lines[1+last_row:]

for line in lines:
    # 原有cells/tItem字段解析不变。
    trdCache["corders"].append(tItem)

return trdCache["corders"][-limit:]
```

委托列表修改后，在返回之前增加：

```python
trdCache["lastrow"] = last_row + len(lines)
return trdCache["corders"][-limit:]
```

成交列表修改前：

```python
for line in lines:
    # 原有cells/tItem字段解析不变。
    trdCache["ctrades"].append(tItem)

return trdCache["ctrades"][-limit:]
```

成交列表修改后：

```python
trdCache["lastrow"] = last_row + len(lines)
return trdCache["ctrades"][-limit:]
```

`lines[1+last_row:]`中的1用于跳过CSV表头。旧版每次都从第0条数据读起，再追加到同一个缓存，所以文件明明只有2条，三次查询却成了2、4、6条。修复后第一次读完把游标变成2，下次没有新行就不重复追加，结果保持2、2、2。此修复针对追加文件；运行中截断/替换同路径CSV的缓存失效规则没有在这一条测试中全面验证。

</details>

### G．调度关闭却存成开启：判断active字段

**Python源码修复。** 文件：`wtpy/monitor/WatchDog.py`，函数：`WatchDog.applyAppConf`。

<details class="source" markdown="1">
<summary>展开修改前、修改后代码</summary>

修改前：

```python
schedule = 'true' if appConf["schedule"] else 'false'
```

修改后：

```python
schedule = 'true' if appConf["schedule"]["active"] else 'false'
```

`schedule`是包含`active/weekflag/tasks`的字典，即使`active=False`，这个字典本身仍非空，旧判断会得到真。修复后以明确开关为准。数据库复核确认本次实时观察的`schedule=false`、`guard=false`；并未借此开启自动拉起或定时下单。

</details>

### H．账号能走原始HTTP登录，却不能网页登录

**初始化脚本与教程纠正，未修改原生鉴权算法。** 旧示例取自首次部署前保存的07篇原文；不是凭空重构某个未留存的bootstrap版本。真实部署使用`DataMgr.add_user()`写入SQLite，而不是注册到WT官网或SimNow，也不是手工编辑数据库文件的字节。

<details class="source" markdown="1">
<summary>展开原示例、正确示例与密码摘要链路</summary>

原教程示例直接把输入的明文交给`add_user`：

```python
from getpass import getpass
from wtpy.monitor.DataMgr import DataMgr

mgr = DataMgr("data.db")
mgr.add_user({
    "loginid": "localadmin", "name": "本机管理员", "role": "superman",
    "passwd": getpass("设置监控密码: "),
    "iplist": "", "products": "", "remark": "本地监控"
}, "bootstrap")
```

正确示例先按原版网页协议生成一次摘要，再交给原生管理函数：

```python
import hashlib
from getpass import getpass
from wtpy.monitor.DataMgr import DataMgr

mgr = DataMgr("data.db")
password = getpass("设置监控密码: ")
if mgr.get_user("localadmin") is None:
    mgr.add_user({
        "loginid": "localadmin", "name": "本机管理员", "role": "superman",
        "passwd": hashlib.md5(password.encode("utf-8")).hexdigest(),
        "iplist": "", "products": "", "remark": "本地监控"
    }, "bootstrap")
```

原生`DataMgr.add_user`内部的这行**没有修改**：

```python
encpwd = hashlib.md5((loginid+usrInfo["passwd"]).encode("utf-8")).hexdigest()
```

所以要让网页登录一致，数据库实际保存的是`MD5(loginid + MD5(明文口令))`。浏览器发来内层MD5，服务端再算外层并比较；用户输入的仍是明文口令。当前bootstrap用`secrets.token_urlsafe(15)`生成随机口令，私密资料写入`login.local.json`，再按上述协议调用`add_user`。`add_user`对已有账号主要改资料，不负责重设密码；不要重复执行示例当作改密工具。

这里说明的是既有实现，不把MD5当成新的安全设计。当前服务只绑定回环地址，经SSH本机转发访问；它不是已经完成对外开放安全改造的账户平台。

</details>

### I．实时日志不滚动：原来是展示配置选择错误

**配置纠正。** 不是MQ函数或策略交易逻辑修复。

<details class="source" markdown="1">
<summary>展开原配置、纠正配置与前端既有判断</summary>

实时组合原配置：

```python
{'id': 'simnow03', 'datmod': 'mannual'}
```

首次修复时纠正为：

```python
{'id': 'simnow03', 'datmod': 'auto'}
```

当时原生前端的既有逻辑是：

```javascript
this.logScroll="auto"==t.datmod
```

这是现有发布JS中组合切换监听器的原句，`t`为新选中的组合；可读写法相当于`this.logScroll = (group.datmod === "auto")`。

手动模式会忽略MQ日志的追加显示；它适用于只读历史文件。`auto`仅改变监控展示，不会给策略传`--trade`，不会创建执行器，也不会开启WatchDog守护。下午已经把用户的暂停/恢复选择与datmod分开，代码与行为见J。

</details>

### J．实时日志也能选择自动刷新

**前端源码修改。** 上游仓库只附带编译后的Vue前端，没有完整Vue源码工程。维护脚本`tools/patch_monitor_log_controls.py`对发布JS做有限、可重现的挂接，实际控制逻辑保留在可读的`monitor-log-controls.js`。没有重写一套监控，也没有改交易接口。

<details class="source" markdown="1">
<summary>展开原界面条件、修改后的开关与请求保护</summary>

原编译JS用下列渲染参数隐藏实时模式的复选框，且收到日志时只判断数据模式：

```javascript
// el-row的原渲染参数（节选）
{directives:[{name:"show",rawName:"v-show",value:!t.logScroll,expression:"!logScroll"}]}
// 原事件处理条件
if(!t.logScroll)return;t.processLog(e)
```

新渲染参数移除上述v-show限制，使原复选框与刷新按钮一直可见；事件处理增加用户选择：

```javascript
// 同一个el-row的参数改为{}，其他子组件不变
if(!window.WtMonitorLogControls.shouldAppend(t))return;t.processLog(e)

// 新增可读控制器中的实际函数
function shouldAppend(view) {
    return !view.logControlsDisposed && view.logScroll && view.autoLog;
}
function reset(view) {
    stop(view);
    if (view.logControlsDisposed) return;
    if (view.autoLog && !view.logScroll) {
        view.logInterval = root.setInterval(function () { view.queryLogs(); }, 15000);
    }
}
```

原`queryLogs`回调不检查组合是否已切换；原组件销毁后，在途回读还可能重建timer。现在浏览器为每次读取记录组合ID和请求序号，组件销毁时标记disposed，迟到结果不能覆盖其他组合，也不能重新启动轮询：

```javascript
function acceptRead(view, request) {
    return !view.logControlsDisposed && request.groupid === view.groupinfo.id &&
        request.serial === view.logControlsRequest;
}
function dispose(view) {
    view.logControlsDisposed = true;
    stop(view);
}
```

完整文件及本次验证见[日志控制源码](F:/XiangMu/Trader/wtpy/wtpy/monitor/static/console/monitor-log-controls.js)、[维护脚本](F:/XiangMu/Trader/wtpy/tools/patch_monitor_log_controls.py)、[行为验证](../evidence/monitor/controls/frontend_control_validation.json)与[页面验收](../evidence/monitor/controls/browser-controls.json)。从Git原始JS重新套用维护脚本，得到的文件与部署文件逐字节一致；没有计算哈希。

</details>
