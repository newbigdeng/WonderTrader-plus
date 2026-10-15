# 原生MQ修复与定向验收

2026-10-15，源码基点：VM WT d816d8c5、wtpy 1c7d3d3，叠加已有本地修改。

`native.patch`修复EventNotifier的work寿命、停止/等待顺序、空函数指针初始化和委托引用释放；`watchdog-mq.patch`修复AppInfo.applyConf重建接收器漏传sink、清空地址未清指针。实际源文件已经修改，补丁用于审阅，不要在已修改源码上重复套用。

`native_mq_probe.cpp`只构造原生事件对象并调用EventNotifier，没有Parser、Trader或Executer，不连接柜台。TEST.MQ.0001不是可交易合约，990001/990002/990003是测试标记。运行参数依次为独立IPC地址、原生库目录、追加命令文件；支持LOG 文本、ORDER、CANCEL、TRADE、QUIT。命令文件必须在进程启动前创建，QUIT后正常析构。

`mq_regression.py`对已经分别链接原/修复版源码的native-mq-before和native-mq-after进行实际MQ订阅；两条日志、两条委托、一条成交在空闲后发送。旧版0条，修复版5条；订单引用计数2→1，析构完成。测试不会重编、重启真实交易或发送柜台订单。

`mq_capture.py`是只读监控WebSocket记录器，使用VM私有登录文件，参数为标签、组合ID、持续秒数；不包含账号密码，不使用交易API。当前路径对应本次已搭建的simnow-03，不是任意机器上的通用安装脚本。

编译沿用现有GCC8的WtPorter目标。旧POST_BUILD复制缺ParserShm，最终直接执行该目标现有link.txt中的原链接参数成功；未覆盖CTP适配库。完整编译日志、原文件、受控输入和失败证据在个人归档及VM保留，没有进行全量回归或哈希验证。

分层结果见[验收记录](../../../evidence/monitor/mq-fix/verification.json)，原因与操作见[13号教程](../../../guides/13_wtpy监控部署与操作.md)。新增CTP开平到监控的整条链路仍待开盘验收。页面部分重复展示尚未定位，不能把提示数当作成交数。
