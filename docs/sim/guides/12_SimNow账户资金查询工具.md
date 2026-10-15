# SimNow 账户资金查询工具

更新：2026-10-14。**程序已经部署到 `simnow-03/account-query`，真实查询成功，资金和请求过程都写入本目录日志。** 它直接使用官方 CTP SDK，不需要先开 Loader、行情中台、WT 引擎或策略。

## 1．怎样使用

在虚拟机终端执行：

```bash
cd /home/zjh/wt-lab/records/simnow-03/account-query
./run.sh
```

一次执行只查询一次人民币账户资金，完成后自动退出。查看结果：

```bash
tail -n 40 account_query.log
cat account_latest.json
```

`run.sh` 会先进入自身所在目录，因此从其他目录调用它，输出仍在 `account-query`。日志文件名不带日期，内容逐次追加，时间保存在每条记录中；JSON 则更新为最近一次请求的结果，读取时同时检查 `result` 与 `queried_at`。

## 2．目录里有什么

```text
account-query/
  AccountQuery.cpp        C++源码，直接调用CTP
  AccountQuery            已编译的Linux程序
  build.sh / run.sh       编译与单次查询入口
  config.ini             独立私密认证配置，权限600
  config.example.ini     不含真实账号密码的模板
  sdk/include/           本次官方SDK的3份头文件
  flow/                  本工具自己的API流文件
  account_query.log      追加查询过程与资金字段
  account_latest.json    最近一次请求的结构化结果
  verification.json     本次实际验收记录
```

配置从现有 `loader/config.ini` 的 `[ctp]` 节提取，包含第一套交易前置 `30003`、Broker `9999`、账号、密码、AppID 和认证码。它不读取 Loader 的合约导出设置，也不会更新 `common`。密码不进入命令行或应用日志。

运行时复用 `../runtime/wtpy/wrapper/linux/traders/thosttraderapi_se.so`，三份编译头文件来自已经下载的 `v6.7.13_20260225` 官方 SDK；没有重新下载、安装或复制另一套运行库。可执行文件的 RPATH 以自身所在目录为基准找到该库。以后改源码时，在本目录运行 `bash build.sh` 即可。

此版本明确限制为现有 SimNow 第一套 `9999/30003`，错误环境会在连接前拒绝。`run.sh` 使用 `.query.lock` 防止同目录同时运行两次；锁不覆盖其他策略进程。本轮没有检验它与另一个同账户交易会话同时运行的情况。

## 3．从调用到回调，实际发生什么

```text
CreateFtdcTraderApi("flow/", true)  使用生产密钥模式
  → RegisterSpi / RegisterFront / Init
  → OnFrontConnected
  → ReqAuthenticate / OnRspAuthenticate
  → ReqUserLogin / OnRspUserLogin
  → ReqQryTradingAccount
  → OnRspQryTradingAccount
  → 最后一个回调完成，主线程Release并写最终JSON、退出
```

核心请求代码：

```cpp
CThostFtdcQryTradingAccountField query{};
copyField(query.BrokerID, cfg["broker"]);
copyField(query.InvestorID, cfg["user"]);
copyField(query.CurrencyID, std::string("CNY"));
queryRequest = ++request;
submitted("ReqQryTradingAccount",
          api->ReqQryTradingAccount(&query, queryRequest), queryRequest);
```

**请求返回 0 只是提交成功，资金要在异步回调里读取。** 程序核对请求 ID、`ErrorID`、资金数据与 `bIsLast`；只有完整收到有效 CNY 记录才写 `RESULT=passed`。最后一个回调即使没有数据指针，也要结合此前收到的记录判断，不能只看最后一条指针。

日志包含连接、认证、登录、请求返回值、每个资金字段、最后结果。CTP 错误文本按 GB18030 转 UTF-8，应用日志使用 UTF-8；字段没有使用“忽略解码错误”的方式丢弃。API 的 `Release` 在主线程执行，避免在 SDK 回调线程里等待自身结束。

本工具没有报单、撤单、结算确认或策略调用。资金查询不需要调用 `ReqSettlementInfoConfirm`，本次直接在登录后查资金已实际通过。它也不查询持仓和活动委托，不能拿资金查询成功代替交易前的全账户对账。

## 4．这些金额应该怎样读

以下含义按本次 SDK 的 `CThostFtdcTradingAccountField` 注释解释。日志保留原字段名，便于与柜台及源码对应。

| 字段 | 含义与区别 |
|---|---|
| `Balance` | SDK称“期货结算准备金”；展示柜台直接返回值，不能一概当成可下单或可取出的余额 |
| `Available` | 可用资金；与保证金、冻结等状态有关 |
| `PreBalance` | 上次结算准备金；不是当前余额 |
| `CurrMargin` / `ExchangeMargin` | 当前保证金总额 / 交易所保证金 |
| `FrozenMargin` / `FrozenCash` / `FrozenCommission` | 冻结保证金、资金、手续费 |
| `CloseProfit` / `PositionProfit` | 平仓盈亏 / 持仓盈亏 |
| `Commission` | 手续费；与盈亏单列 |
| `Deposit` / `Withdraw` | 入金 / 出金 |
| `WithdrawQuota` | 可取资金；不等同于 Available |
| `CurrencyID` / `TradingDay` | 币种 / 柜台交易日；夜盘可不同于本地自然日 |

金额查询是当次柜台快照，不是实时推送。需要刷新就重新执行；当前没有后台定时轮询。日志展示6位小数，JSON保存较高精度的原始浮点值。

## 5．这次实测结果与错误处理

2026-10-14 **22:33:02 +0800** 查询完成，程序退出码 0，回调 `bIsLast=1`，账户交易日 **20261015**：

| 字段 | 实际返回值（元） |
|---|---:|
| Balance | 19,999,967.744 |
| Available | 19,999,967.744 |
| PreBalance | 20,000,000 |
| CurrMargin / PositionProfit | 0 / 0 |
| CloseProfit | -20 |
| Commission | 12.256 |
| WithdrawQuota | 15,999,974.19 |

这个空仓场景中，`20,000,000－20－12.256 = 19,999,967.744`，与11号报告的两轮开平损益对应。只解释本次零持仓、零冻结、零入出金场景，不把这个等式当作所有账户状态的通用资金公式。

| 检查 | 结果 |
|---|---|
| 编译、运行库依赖、真实查询与日志/JSON | 通过；最终编译无警告，真实查询退出0 |
| 错误Broker配置 | 连接前拒绝，退出2；失败JSON为空记录，没有伪造零余额 |
| 同目录已有查询锁 | 第二次启动退出73，没有再次登录或查询 |
| 私密配置与日志 | config.ini权限600，应用日志不含账号或密码 |

柜台错误、空资金回报、超时40秒或中断属于请求失败，退出1，并更新失败JSON；配置或输出错误退出2，日志记录 `FATAL`。成功日志和结构化结果应一起看。SDK 自身可能在终端输出设备信息采集权限提示，本次这些提示没有阻止认证与资金查询；应用日志以完整资金回调和最终结果判定成功。

超时、中断和断网分支已有实现，本轮没有分别注入这些故障。此程序只用于 Linux VM，不声称 Windows版已经测试，也没有发送新订单或重跑整个 WT 回归。

源码见[AccountQuery.cpp](../tools/account-query/AccountQuery.cpp)，使用说明见[程序README](../tools/account-query/README.md)。本次[验收记录](../evidence/account-query/verification.json)、[查询日志](../evidence/account-query/account_query.log)和[资金JSON](../evidence/account-query/account_latest.json)均为当次副本；最新结果仍以 VM 工具目录为准。

回到[11 在线链路验收](11_simnow03在线链路验收.md)或[阅读目录](README.md)。
