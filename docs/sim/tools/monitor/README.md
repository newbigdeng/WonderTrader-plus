# simnow-03 原生监控部署文件

这些是VM已验证入口与隔离修复记录，不包含私密账号、SQLite或依赖环境。VM目录为`simnow-03/monitor`；启动`./start_monitor.sh`，Windows通过SSH转发访问`http://127.0.0.1:8099/console/index.html`。

`run.py`实际位于VM的`simnow-03/run`，固定启动原有观察模式；不是监控服务入口。`bootstrap.py`登记一个实时组合和两个真实归档快照，并创建本机网页登录账号。原生UI的登录摘要协议在初始化时保持一致。

`runtime-fixes.patch`记录已应用到simnow-03专属wtpy副本的缓存游标、调度布尔值修复；这些是首次部署时的隔离修复。后续已修改WT EventNotifier和wtpy主源码的WatchDog MQ重配路径；真实日志与受控原生回报推送通过，新增CTP开平推送待验收，详见[native-mq](native-mq/README.md)。完整点击方法、数据边界和启动说明见[13号教程](../../guides/13_wtpy监控部署与操作.md)。
