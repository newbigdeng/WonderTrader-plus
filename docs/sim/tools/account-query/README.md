# 独立 SimNow 资金查询程序

部署目录：VM `/home/zjh/wt-lab/records/simnow-03/account-query`。

```bash
cd /home/zjh/wt-lab/records/simnow-03/account-query
./run.sh
```

每次执行认证、登录、一次 `ReqQryTradingAccount` 查询；等 `OnRspQryTradingAccount` 最后一个回调后释放 API 并退出。

本目录 `account_query.log` 追加过程与资金字段，`account_latest.json` 保存最近一次完成请求的结构化结果。默认查 CNY。`return=0` 只表示请求提交成功，最终应看 `RESULT=passed`。不启动 WT 引擎，也不接行情、报单或确认结算。

程序复用上级 `runtime/wtpy/wrapper/linux/traders/thosttraderapi_se.so`；官方三份头文件保存在本目录 `sdk/include`，需要重编译时执行 `bash build.sh`。私密 `config.ini` 已从已有 Loader 配置提取，权限 600，密码不进日志。`flow` 独立；`run.sh` 使用文件锁避免此目录并发登录。

超时40秒或柜台回报错误会记录失败，退出码1；配置或输出文件错误退出码2；同目录运行冲突退出码73。此工具只查询资金，不能证明没有持仓或未完成订单。不能用资金为零与否代替订单/持仓对账。

字段含义与实测结果见个人目录的12号HTML。此仓库副本不包含密码、SDK、二进制或API私密流文件。
