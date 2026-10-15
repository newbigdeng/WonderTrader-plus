# wtpy 监控与 SQLite

核对日期：2026-10-15。以 VM 当前运行源码及配套 wtpy 为依据；代码是关键摘录，省略部分以文字说明。模拟盘真实验收与架构解释分别表述。

<!-- WT-ILLUSTRATION:04_monitor_relations:START -->
<figure class="wt-illustration">
<a class="wt-image-link" href="../images/04_monitor_relations.png" title="点击打开原图"><img src="../images/04_monitor_relations.png" alt="监控三种数据源" loading="lazy" decoding="async"></a>
<figcaption><strong>监控三种数据源</strong>。先记住：SQLite管登录和组合配置，JSON/CSV提供展示数据，MQ提供事件通知。界面是概念示意；右下角对账表示恢复应有的职责，不代表当前Python带仓恢复或多人权限已经验收通过。实际配置与验收状态以正文为准。</figcaption>
<p class="wt-original"><a href="../images/04_monitor_relations.png">打开原图，放大阅读</a></p>
</figure>
<!-- WT-ILLUSTRATION:04_monitor_relations:END -->

## 1．交易数据恢复与监控恢复是两件事

Python CTA 复用 `CtaStraBaseCtx`。初始化时先 `init_outputs()`，再 `load_data()`、`load_userdata()`，最后 `ExpCtaContext` 才回调 Python `on_init`。Python `context.user_save_data` 保存可转为字符串的策略自定义值，`user_load_data` 读取它们；对应 `generated/userdata/ud_策略名.json`。

| 输出 | 主要用途 |
|---|---|
| `generated/marker.json` | 策略、通道、执行器 ID 清单 |
| `generated/stradata/策略名.json` | 策略理论持仓、资金等运行状态 |
| `generated/userdata/ud_策略名.json` | 策略主动保存的冷却、日内计数等自定义状态 |
| `generated/outputs/策略名/*.csv` | 策略信号、理论成交、平仓回合、资金 |
| `generated/portfolio/datas.json` 和 CSV | 组合状态与汇总表现 |
| `generated/execdata/` | 执行器状态，与目标落实相关 |
| `generated/traders/通道/rtdata.json` | 交易通道资金、持仓快照 |
| `generated/traders/通道/orders.csv,trades.csv` | 真正的委托/成交回报记录 |

监控启动不会让持仓自动恢复或主动平仓。策略、执行器读取各自状态，CTP 通道登录后向柜台查询，两者需核对并按明确规则同步。监控只负责展示和管理它已经能找到的数据。


## 2．WtMonSvr 怎么启动，SQLite 到底保存什么

运行监控需要独立 Python 进程、同一文件系统中的组合输出，以及 `libWtMsgQue.so`；它不必与策略处于同一个 Python 进程。启动示例：

```python
# monitor/run_monitor.py
from wtpy.monitor import WtMonSvr

svr = WtMonSvr(deploy_dir="../")
svr.run(port=8099, host="127.0.0.1", bSync=True)
```

从 monitor 目录执行 `python run_monitor.py`，浏览器访问 `http://127.0.0.1:8099/console/`。如果监控在 VM 而浏览器在 Windows，可使用 `ssh -L 8099:127.0.0.1:8099 zjh@192.168.29.128` 转发后访问同一地址。`deploy_dir` 是界面部署目录树的根；它不等于 SQLite 路径，也不会自动注册其中全部策略组合。

**实际 SQLite 文件是 monitor 进程当前目录中的 `data.db`。** 单独创建 DataMgr 的默认值才叫 `mondata.db`，不能混淆。构造函数已经证明：

```python
# wtpy/monitor/WtMonSvr.py:320
self.__data_mgr__ = DataMgr('data.db', logger=self.logger)
self._dog = WatchDog(sink=self,
                     db=self.__data_mgr__.get_db(), logger=self.logger)
```

| SQLite 表 | 实际内容 | 重启后加载的对象 |
|---|---|---|
| users | 登录名、密码摘要、角色、可见组合 products、IP 列表等 | 监控用户配置 |
| groups | 组合 ID、部署 path、类型、环境、MQ URL 等 | 监控去哪里找文件、订阅谁 |
| schedules | 程序路径、cwd、参数、守护与定时任务、MQ URL | WatchDog 的进程管理配置 |
| actions | 登录、改配置、启停等管理操作审计 | 管理日志 |

没有行情 Tick 表、柜台持仓账本或 CTA 交易记录表。数据库中一条 users 记录，不会自动恢复该用户的策略仓位。

默认先走网页登录 `POST /mgr/login`：DataMgr.get_user 查账户，浏览器先发送 `MD5(输入口令)`，服务端再比较 `MD5(loginid + 前端摘要)`；成功后把角色、可见组合、登录时间、过期时间写入 Session，后续 HTTP 读取 Session。SessionMiddleware 使用签名 cookie `WtMonSvr_sid`；cookie 的职责是网页登录身份，与 CTP 认证 AppID/AuthCode、柜台账户无关。源代码也有内建 `superman` 账户。部署可以显式创建自己的监控管理员，避免依赖未知的默认口令。

首次创建管理员可在 monitor 启动前单独运行以下代码，密码由终端输入；这是监控账号，不是期货密码：

```python
from getpass import getpass
import hashlib
from wtpy.monitor.DataMgr import DataMgr

mgr = DataMgr("data.db")
password_digest = hashlib.md5(getpass("设置监控密码: ").encode("utf-8")).hexdigest()
mgr.add_user({
    "loginid": "localadmin", "name": "本机管理员", "role": "superman",
    "passwd": password_digest,  # 对齐原生前端协议，网页登录仍输入原口令
    "iplist": "", "products": "", "remark": "本地监控"
}, "bootstrap")
```

这个初始化脚本只应在尚未创建 localadmin 时使用；DataMgr.add_user 对已有用户是修改资料，并不会重设密码。创建后启动/重启 WtMonSvr，它从 SQLite 重新载入 users。源码的密码存储是旧式 MD5，Session 密钥也写死，因此以上先作为本机监控使用的启动方式，不把它当作已经完善的对外账号安全系统。本轮未改鉴权代码。

网页登录后新增组合，例如 ID `simnow03`，path 为监控进程能访问的 py_runner **绝对路径**，类型 `cta`、环境 `product`。监控的 DataMgr 会读取：

```python
# wtpy/monitor/DataMgr.py
filepath = os.path.join(grpInfo["path"], "./generated/marker.json")
filepath = os.path.join(grpInfo["path"],
                        "./generated/traders/%s/rtdata.json" % cid)
```

它从 marker 找到策略/通道 ID，从 CSV、JSON 读取资金、持仓、交易等。源码固定查找 `generated`，自定义 `genDir` 后不能假设监控会自动跟随。运行目录在容器、监控在宿主机时，应给 SQLite 里的 group.path 写宿主机挂载后的对应路径，而不是只在容器里存在的 `/work/...`。


## 3．监控实时通知、行情广播与重启恢复

监控实时事件用单独的 MQ：

```text
交易内核 EventNotifier（notifier.url）
 → WtMsgQue / libWtMsgQue
 → WatchDog.AppInfo / EventReceiver（groups/schedules 的 mqurl）
 → WtMonSvr.on_order/on_trade/on_output
 → PushServer 队列、WebSocket
 → 浏览器实时通知
```

主要 topic 为 `TRD_ORDER`、`TRD_TRADE`、`TRD_NOTIFY`、`LOG`、`TIMEOUT`。不要把它们与行情 UDP 或共享内存无锁队列混淆。MQ URL 可以是 `ipc:///...` 或适合当前环境的其他协议；发布端和订阅端须使用同一地址。跨宿主机/容器时 IPC 文件还需共享挂载，否则字符串相同仍不在同一文件系统。

只登记 group.path 可以读历史文件；要看实时推送还需要对应的 MQ URL，并让 WatchDog 的应用配置或对现有进程的识别建立 EventReceiver。WatchDog 可提供启停、守护和时段调度，它实际调用 `subprocess.Popen(args, cwd=folder)`；新部署先关闭自动守护/调度，避免用户手工启动与自动拉起重叠。

实时日志还要求组合的`datmod=auto`；`mannual`模式按原设计忽略日志推送。普通委托事件能传输，但本版前端只弹撤单与成交通知，表格仍读文件。2026-10-15的修复、实际代码及分层证据见[13号](13_wtpy监控部署与操作.md)。

重启监控时，SQLite 恢复“有哪些账号、组合在哪里、管理哪一个进程”；JSON/CSV恢复“界面上展示哪些历史结果”；MQ提供重启之后的实时通知。真正交易恢复由交易引擎、策略状态与柜台查询完成。SQLite 用户不会把之前网页上看到的仓位重新写回柜台。


## 4．回测监控有两种入口

仅浏览已经完成的本地回测，使用 `WtBtSnooper`：

```python
from wtpy.monitor import WtBtSnooper
snooper = WtBtSnooper()
snooper.run_as_server(port=8081, host="127.0.0.1", bSync=True)
```

打开 `http://127.0.0.1:8081/`，登记 outputs_bt 的绝对路径作为工作空间，读取 `策略名/funds.csv,trades.csv,closes.csv,signals.csv,summary.json,btenv.json`。这一路工作空间保存到当前目录 `data.json`，不使用 WtMonSvr 的 users/groups SQLite 登录流程；它主要查看已有结果，不能只靠它就获得在线回测状态。

需要浏览器管理策略、启动回测并看进度时，在 WtMonSvr 上附加：

```python
from wtpy.monitor import WtMonSvr, WtBtMon
svr = WtMonSvr(deploy_dir="../")
svr.set_bt_mon(WtBtMon(deploy_folder="./bt_deploy", logger=svr.logger))
# 若要图表中的本地历史行情，再配置 WtDtServo，并 svr.set_dt_servo(dtServo)
svr.run(port=8099, host="127.0.0.1", bSync=True)
```

`WtBtMon.run_backtest` 会生成 `bt_deploy/用户/策略ID/backtests/回测ID/`，复制策略、入口和模板配置，启动独立 Python 回测进程；结果在其 `outputs_bt/回测ID/`。用户在此是网页研究工作空间归属，仍不是期货交易账户。WtBtMon 的 `用户/marker.json` 保存策略与回测清单，`tasks.json` 保存正在管理的回测任务；重启时核对进程仍存在则重新接管任务，否则更新已经完成的结果。该恢复不会从某一根 K 线继续未完成回放。

当前 `WtBtMon.run_backtest` 硬编码模板文件名为 `configbt.json/logcfgbt.json/fees.json/runBT.py`，而示例目录中的模板有 YAML 文件；不能只把 demo 原样复制并写 set_bt_mon 就认为在线回测已可用。需要按源码要求准备模板、基础数据路径、MQ和依赖，再单独验证。`BT_EVENT/BT_STATE/BT_FUND` 通知属于回测进程实时状态。


2026-10-13首次核对时，simnow-03尚缺fastapi，监控未启动验收。2026-10-15已在独立monitor/.venv安装Web依赖，登记实时观察与两个交易档案，网页登录和32项只读检查通过；随后定位并修复了原生通知器空闲断流，真实CTP日志与受控原生回报推送到页面通过；新增CTP开平推送仍待开盘验收。实际入口、点击方法、源码修复及边界见[13 部署与操作](13_wtpy监控部署与操作.md)。不要在已运行环境重复创建账号或覆盖数据库。

2026-10-15下午：日志区已增加可自行勾选的自动刷新；监控页面逐项读写、账号创建、非交易快照边界见[14号](14_监控原理与页面读写链路.md)，原/新源码对照见[13号](13_wtpy监控部署与操作.md)。通道CSV游标及调度active修复现已同步到wtpy主源码。
