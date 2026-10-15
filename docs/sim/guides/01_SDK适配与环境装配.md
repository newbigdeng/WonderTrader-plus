# SDK 适配与环境装配

这次使用的包是 **CTP v6.7.13_20260225、Linux x86_64**。这是实际下载并验证的版本，不把它写成永远最新。WT 基础为 `c7c5619` 加此前本地修复；适配代码位于虚拟机 `simnow-01/adapter-src`，尚未合并为正式跨平台版本。新 `simnow-03` 复用这一适配。

## 1. 为什么不能只替换一个 .so

WT 的 CTP 插件通过 `dlopen/dlsym` 动态找 API 创建函数，同时按照 CTP 头文件编译字段和 SPI 回调。新库必须与头文件、函数指针和调用参数配套。头文件中的默认参数属于编译期，不能让旧导出符号自动兼容新库。

实际创建函数发生了以下变化：

| 接口 | 原 6.3.15 | 本次 6.7.13 |
|---|---|---|
| 行情 | `CreateFtdcMdApi(path, udp, multicast)` | 再加 `bIsProductionMode` |
| 交易 | `CreateFtdcTraderApi(path)` | 再加 `bIsProductionMode` |
| Linux 行情符号尾部 | `EPKcbb` | `EPKcbbb` |
| Linux 交易符号尾部 | `EPKc` | `EPKcb` |

SimNow 是仿真业务环境，但用户提供的前置说明要求生产密钥，因此新参数传 **true**。这个参数选择 API 的密钥模式，不把 SimNow 变成真实资金账户；订单去哪里仍由交易前置和账户决定。

## 2. 哪些源码改了，为什么

| 位置 | 实际改动 |
|---|---|
| `ParserCTP.h` | 头文件改指向行情新 SDK；创建函数指针由三参数变四参数 |
| `ParserCTP.cpp` | 更新符号名；用四参数创建；库、符号和 API 空值检查；相关成员指针初始化为空 |
| `TraderCTP.h` | 头文件指向交易新 SDK；创建函数指针增加 bool |
| `TraderCTP.cpp` | 更新符号；创建时传 true；补失败保护和成员指针初始化 |
| `CTPLoader.cpp`、`TraderSpi.h` | 合约查询也改用新交易 SDK；符号、参数与失败返回同步修改 |
| 独立 CMake | 只编译上述三个插件，公共 WT 头文件引用原仓库 |

行情创建路径的核心变化如下，省略了路径拼接和日志：

```cpp
// 原接口
typedef CThostFtdcMdApi* (*CTPCreator)(const char*, const bool, const bool);
m_pUserAPI = m_funcCreator(path.c_str(), false, false);

// 新接口：函数指针和调用一起改
typedef CThostFtdcMdApi* (*CTPCreator)(const char*, const bool, const bool, bool);
if (!m_hInstCTP || !m_funcCreator) return false;
m_pUserAPI = m_funcCreator(path.c_str(), false, false, true);
if (!m_pUserAPI) return false;
```

交易侧原来单参数，现在两参数。下面对照来自当时留存的`ctp6713-adapter-v2.patch`，涉及头文件、`init`和`connect`三个位置；不是本轮重新改动SDK。

<details class="source" markdown="1">
<summary>展开交易API创建函数修改前后代码</summary>

修改前：函数指针、Linux符号与创建调用只包含flow路径。

```cpp
// TraderCTP.h
typedef CThostFtdcTraderApi* (*CTPCreator)(const char *);

// TraderCTP.cpp::init，Linux分支
const char* creatorName =
    "_ZN19CThostFtdcTraderApi19CreateFtdcTraderApiEPKc";
m_funcCreator = (CTPCreator)DLLHelper::get_symbol(m_hInstCTP, creatorName);

// TraderCTP.cpp::connect，原有flow目录拼接之后
m_pUserAPI = m_funcCreator(ss.str().c_str());
```

修改后：函数签名、符号和调用增加同一个bool参数，并检查加载/创建结果。

```cpp
// TraderCTP.h
typedef CThostFtdcTraderApi* (*CTPCreator)(const char *, bool);

// TraderCTP.cpp::init，Linux分支
const char* creatorName =
    "_ZN19CThostFtdcTraderApi19CreateFtdcTraderApiEPKcb";
m_funcCreator = (CTPCreator)DLLHelper::get_symbol(m_hInstCTP, creatorName);
if (!m_funcCreator) return false;

// TraderCTP.cpp::connect，原有flow目录拼接之后
if (!m_funcCreator) return;
m_pUserAPI = m_funcCreator(ss.str().c_str(), true);
if (!m_pUserAPI) return;
```

`init`返回bool，`connect`返回void，因此两处失败保护的返回语句不同。这是三个位置的摘录，不能合并成一个函数直接覆盖源码。`CTPLoader`使用同一个交易API，它的函数指针、Linux符号与`g_ctpCreator(flowPath.c_str(), true)`也同步修改。`true`选择本次前置要求的生产密钥模式，仍由SimNow前置和账户决定仿真业务环境。

</details>

初始化失败保护必须配合构造函数成员置空。否则核心继续尝试连接时，未初始化的函数指针可能被当成有效地址。完整实际补丁另附在个人源码材料中，上面是讲解用的缩短片段，不能整段直接覆盖原函数。

新头文件使当前用到的结构体和回调重新编译；本次没有重写所有订单业务，也没有完成长合约、期权、所有交易所与 Windows 的兼容验证。Linux 隔离副本明确拒绝 Windows 构建。

## 3. 动态库需要放在哪里

插件自身的 Linux 名称通常带 `lib`，CTP 原生库按当前插件代码的约定不带：

```text
datahub/parsers/libParserCTP.so
datahub/parsers/thostmduserapi_se.so
run/traders/libTraderCTP.so
run/traders/thosttraderapi_se.so
loader/libCTPLoader.so
loader/thosttraderapi_se.so
```

之前曾把原生库放错到进程根目录，这一装配错误已经纠正。ParserCTP/TraderCTP 以插件模块目录定位原生库；Runner 的进程 cwd 与插件自身位置是两种路径概念。wtpy 的对应位置是专属包的 `runtime/wtpy/wrapper/linux/parsers`、`traders`，详见配置篇。

## 4. simnow-03 怎样变干净

我们复用已有编译工具链与 Python 环境，重新建运行目录，不复制旧账户状态。只复制必要 Python 源文件、相配的桥库、插件和配置；基础数据限定为 `rb2701`，因此不会把其他 19 个未完成的品种映射带进这个策略的启动范围。

| 输入 | 来源与处理 |
|---|---|
| wtpy Python 包 | VM 已改造版本，含三种成交模型参数 |
| `WtPorter` | 当前源码单独构建的运行桥库 |
| `WtBtPorter` | 当前源码重新构建，导出 v2/v3；旧 Release 桥库曾不匹配 wrapper，不能继续使用 |
| ParserCTP/TraderCTP/CTPLoader | 已验证的新 SDK 隔离适配产物 |
| Reader/Writer、ParserUDP、执行器 | 同一 GCC8 构建目录所需的模块 |
| `common` | 已查询基础文件中只取 rb2701 和 rb 品种；会话、日历保留必要模板 |
| `generated`、CTP flow、FUT_Data | 新建，未继承旧订单、持仓或会话 |
| 历史诊断数据 | 另建 storage 与 common_historical，不当作当前账户状态 |

`WtPorter` 的 CMake 自动复制阶段因缺少未使用的 ParserShm 失败；编译已完成，随后复用已生成的链接脚本链接桥库，再按实际需要装配插件。这是打包处理，不把它记为全构建通过或源码修复。

## 5. 已通过的范围

2026-10-13 第一套 30003/30013 认证、查询、交易通道 ready、连续 240 条真实 Tick 落盘及有限正常退出已通过。新 simnow-03 本轮只运行离线回测和无通道的 WtEngine 初始化，**没有接 CTP，也没有发单**。Python 版带仓退出不能直接继承 C++ WtRunner 的退出验收结论。

以后更换 SDK，仍要先核对头文件签名、导出符号、原生库平台与实际加载位置，再验证认证、查询、行情和订单业务。无需因一个接口变化重写整套策略或数据系统。
