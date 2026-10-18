# 为什么保留这两套头文件？

原生代码仍使用旧日志和 JSON 接口。
这里保留编译兼容头，避免混用接口。

| 目录 | 版本 | 来源与许可 |
| --- | --- | --- |
| include/spdlog | 1.3.1 | spdlog，Gabi Melman，MIT |
| include/rapidjson | 1.0.2 | RapidJSON，Tencent / Milo Yip，MIT |

每个原文件的版权头均原样保留。
部分 RapidJSON 文件另有第三方许可。
对应说明位于这些原文件的开头。

这些头文件不代表依赖全部升级。
Boost、Asio、NNG 来自 Ubuntu 系统包。
CTP 头文件与运行库采用同一套 6.7.13。

完整许可保留在 LICENSE.spdlog、LICENSE.rapidjson。

原版许可地址：

- https://github.com/gabime/spdlog/blob/v1.3.1/LICENSE
- https://github.com/Tencent/rapidjson/blob/v1.0.2/license.txt
