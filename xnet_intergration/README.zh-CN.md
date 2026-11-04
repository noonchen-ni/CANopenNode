# NI-XNET CANopen 设备模拟器

[English](README.md) | 简体中文

> 本文是 [README.md](README.md) 的中文译本。如两者存在差异，以英文版为准。

这是一个围绕现有 CANopenNode C 协议栈的 C++17 共享库封装，面向 NI Linux RT
AMD64（x86-64）和 VeriStand 自定义设备（Custom Device）。所有集成源代码均位于本目录中，
未修改任何现有 CANopenNode 源文件。

每个句柄拥有一个模拟设备、一个物理 XNET 接口、两个原始帧流（frame-stream）会话、
一个运行时对象字典（Object Dictionary, OD）以及一个处理工作线程（worker）。
库自身负责全部帧 I/O，XNET 不需要 CAN 数据库。

本 README 是该集成的维护与设计参考。[公共 C 头文件](include/xnet_canopen.h)
定义了确切的 API 契约；实现和测试定义了受支持的行为。历史 API 草案、实现进度报告
以及已被取代的设计讨论不属于本指南的内容。

## 目录

- [设计背景与职责边界](#design-background-and-responsibility-boundaries)
- [文件](#files)
- [构建](#build)
- [生命周期与所有权](#lifecycle-and-ownership)
- [JSON 模型版本 1](#json-model-version-1)
- [批量 ABI](#batch-abi)
- [TPDO、故障与错误](#tpdo-faults-and-errors)
- [文件日志](#file-logging)
- [LSS 与持久化配置](#lss-and-persistent-configuration)
- [外部设备模型集成](#external-device-model-integration)
- [Kinco 配置与兼容性说明](#kinco-configuration-and-compatibility-notes)
- [维护与扩展集成](#maintaining-and-extending-the-integration)
- [验证与限制](#validation-and-limits)

<a id="design-background-and-responsibility-boundaries"></a>

## 设计背景与职责边界

目的是为外部主站模拟一个 CANopen 受控节点，由 VeriStand 提供应用行为和通道。
CANopenNode 提供的是通信功能，而不是电机/被控对象模型或 CiA 402 状态机。
添加驱动器对象或导入 EDS 并不会实现驱动器行为。主机运行时状态、CANopen NMT 状态
和 CiA 402 驱动器状态是三个独立的概念：NMT Operational 并不意味着驱动器处于
Operation Enabled。

```text
External CANopen master
    <-> classic CAN bus <-> NI hardware / NI-XNET
    <-> library-owned adapter / CANopenNode / runtime OD
    <-> C batch, status, TPDO-request and fault APIs
    <-> VeriStand adapter / channels
    <-> external device / CiA 402 / plant model
```

| 层 | 职责 |
| --- | --- |
| NI-XNET 适配器 | 在一个物理接口上拥有两个原始帧流会话；接收/过滤帧、排队输出、应用波特率变更，并暴露总线/队列故障。 |
| CANopenNode 集成 | NMT、SDO 服务器、固定 RPDO/TPDO、心跳或节点/寿命守护、SYNC 消费者、EMCY 生产者、LSS 从站、OD 存储、生命周期和状态。 |
| 配置供给 / 通道清单 | 在初始化前生成带版本的 JSON；从同一个一致的模型描述 OD 地址、类型、权限、原始默认值、固定 PDO 布局和通道编码。 |
| VeriStand 适配器 / 外部模型 | 调度模型执行，解释命令，实现所选设备模式、运动、I/O 优先级、缩放、反馈、报警和故障复位行为。 |
| 可选的设备专用处理程序 | 仅在测试契约选定时解释厂商命令；需要时使用通用集成钩子，而不是将 Kinco 的语义嵌入通信核心。 |

重要的设计选择：

- 部署目标是 **NI Linux RT AMD64（x86-64）**。Windows 和桌面 Linux 模拟（mock）构建
  是开发工具，不代表部署/硬件认证。
- 每个句柄拥有一个设备和一个接口。LabVIEW 持有句柄而不是 XNET 会话，既不传递帧，
  也不调用协议处理步骤。接口/路径的独占性由调用方管理，而不是由库内注册表强制。
- 在初始化时一次性构建 OD，而不是为每个客户生成 C 代码，或在运行时调整大小。
  值可以改变，但描述符和存储地址在反初始化之前必须保持稳定。
- PDO 映射、使能状态和通信记录在每次初始化中是固定的。请配置主站，使其不依赖
  于启动时对这些记录的 SDO 写入。实时 SYNC/EMCY/守护设置是另外支持的功能。
- VeriStand 访问通常为 1000 Hz，采用同步的最新值交换。该契约不保证每个命令边沿、
  下一周期生效或同一 SYNC 周期内的反馈。CAN 通信速率与通道轮询速率相互独立。
- 完整的 LSS 从站集成包括适配器波特率激活和基于文件的存储，而不仅仅是节点 ID 分配。
  不支持的操作和失败不得被确认为成功。
- 不承诺硬实时截止期限。先前观察到的约 200 us 的 XNET 写入到发送延迟是特定条件下
  的测量值，并非最坏情况上界，也不是模型到总线的总延迟。

### 维护所需的 CANopen 概念

本移植使用经典 CAN：标准 11 位标识符，最多 8 字节负载，不使用 CANopen FD 或
29 位帧。OD 地址由 16 位索引和 8 位子索引组成，例如 `0x1018:01`（厂商 ID）。
OD 值保持在本地，除非通过协议服务或主机 API 访问。

| 服务 | 本地模拟设备的角色 |
| --- | --- |
| SDO | 响应远程 OD 读写的服务器；应答主站不需要 SDO 客户端。 |
| PDO | TPDO 生产者和 RPDO 消费者，以模拟节点的视角命名。 |
| NMT | 受控节点；Pre-operational 允许 SDO，但不允许 Operational 下的 PDO 交换。 |
| 心跳 / 守护 | 心跳生产者或守护从站；没有心跳消费者/守护主站。 |
| SYNC / EMCY / LSS | SYNC 消费者、EMCY 生产者和 LSS 从站。 |

一个 XNET 流会话承载多个协议 CAN ID；PDO 不是会话。TPDO/RPDO 编号是本地槽位号，
而不是总线范围的标识符。生产者和消费者必须在 CAN ID 和负载布局上达成一致；它们的
本地 PDO 编号和 OD 地址不必相同。通常一个 PDO ID 只有一个生产者。

一次 SDO 传输可以跨越多帧：加速（expedited）传输最多携带 4 个值字节，分段传输
使用连续的、每段最多 7 字节的分段。本移植启用分段 SDO，不启用块（block）SDO。
对于节点 10，默认 SDO 请求/应答 ID 为 `0x60A`/`0x58A`；启动报文和心跳使用
`0x70A`。服务器和适配器处理这些交换，无需应用调用 SDO 客户端函数。

运行时 OD 是实时存储/元数据。EDS/XDD 为工具描述能力；DCF/XDC 描述已配置的设备。
原始 XNET 帧 I/O 不需要 DBC。JSON 配置和 EDS 转换不会合成应用行为或工程单位转换。

<a id="files"></a>

## 文件

- [include/xnet_canopen.h](include/xnet_canopen.h)：公共 C ABI。
- [src/model.cpp](src/model.cpp)：版本 1 JSON 校验和 OD 构建。
- [src/driver.cpp](src/driver.cpp)：XNET 会话和 CANopen 驱动边界。
- [src/device.cpp](src/device.cpp)：生命周期、工作线程、LSS、通道、诊断。
- [src/internal.hpp](src/internal.hpp)：私有存储、所有权、绑定和同步。
- [src/CO_driver_target.h](src/CO_driver_target.h)：共享的协议栈功能掩码、帧布局和锁宏。
- [src/persistence.cpp](src/persistence.cpp)：采用原子替换的本地 JSON 存储。
- [src/logging.cpp](src/logging.cpp)：私有 spdlog 文件后端和每设备标签。
- [src/logging_config.h](src/logging_config.h)：编译期固定的日志路径。
- [examples/device.json](examples/device.json)：完整可用的模型。
- [tools/eds_to_json.py](tools/eds_to_json.py)：EDS 到 JSON 模板转换器（Python 3.10+，无第三方包）。
- [examples/kinco.template.json](examples/kinco.template.json)：生成的 Kinco 配置模板，不是已调试完成的模型。
- [examples/kinco.template.inputs.md](examples/kinco.template.inputs.md)：生成的字段输入说明和转换警告；与模板一起保留。
- [tests/](tests/)：模拟 XNET API、真实协议栈测试以及 C 头文件/ABI 检查。
- [tests/app/](tests/app/README.md)：动态加载共享库的独立从站应用、运行配置和硬件冒烟测试说明。

提供的 NI 头文件、JSON 单头文件和内置的 spdlog 头文件均未经修改使用。spdlog
及其捆绑的 fmt 以纯头文件方式编译；部署目标上不需要额外的 spdlog/fmt 共享库。

<a id="build"></a>

## 构建

需要 CMake 3.16+、支持 `std::filesystem` 的 C99/C++17 工具链（推荐 GCC 9+）、
线程库以及目标平台的 NI-XNET 共享库。可在目标机上本地构建，或使用你的
NI Linux RT AMD64 CMake 工具链。请使用目标平台的 AMD64 NI-XNET 库和兼容的
C++ 运行时；仅与桌面主机 CPU 架构相同并不能保证 NI Linux RT 兼容性。

在构建机上从本目录执行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DXNET_LIBRARY=<absolute-path-to-target-XNET-shared-library>
cmake --build build
```

交叉编译时，还需传入
`-DCMAKE_TOOLCHAIN_FILE=<your-NI-Linux-RT-AMD64-toolchain-file>`。
输出为 `libxnet_canopen.so`。部署时需附带与之匹配的 NI-XNET 和 C++ 运行时依赖。
CMake 配置阶段不会访问真实硬件。

与硬件无关的测试：

```sh
cmake -S . -B build-tests -DXNET_CANOPEN_BUILD_LIBRARY=OFF \
  -DXNET_CANOPEN_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

模拟测试也支持 Windows/MSVC，仅用于开发，不用于部署。Linux GCC 构建还可以在
C、C++ 和可执行文件链接器标志中使用 `-fsanitize=address,undefined`。

### 独立设备应用

默认同时构建 `xnet_canopen_device`，可用 `XNET_CANOPEN_BUILD_APP=OFF` 禁用。
应用接受 `--lib` 和 `--config`，每 500 ms 输出全部十个状态字段，并在收到
SIGINT/SIGTERM 时清理资源。可选的 `--duration <seconds>` 用于限时冒烟测试。
运行配置指定 XNET 接口、现有模型 JSON 路径和持久化路径；相对路径基于运行配置文件
所在目录解析。独立构建、配置示例、部署要求和模拟测试见
[tests/app/README.md](tests/app/README.md)。该应用不实现驱动器模型。

<a id="lifecycle-and-ownership"></a>

## 生命周期与所有权

1. `xnet_canopen_create(&handle)`。
2. `xnet_canopen_initialize(handle, interface, model_path, persistence_path)`。
3. 绑定通道组。
4. `xnet_canopen_start(handle)`。
5. 读写通道、查看状态、请求 TPDO 或注入故障。
6. 需要停止通信时调用 `xnet_canopen_stop(handle)`。
7. 加载另一个模型之前调用 `xnet_canopen_uninitialize(handle)`。
8. `xnet_canopen_destroy(handle)`；必要时它也会执行停止/反初始化。

所有路径/名称字符串在初始化期间被复制，不做规范化处理。调用方负责提供正确的接口
名称和路径。请为每个设备提供一个明确的可写持久化文件路径。在存储 LSS 配置之前，
其父目录必须已经存在；文件本身无需存在。

初始化成功并不会启动通信。已经运行时 `start()` 为空操作；重复调用 `stop()` 会保持
停止状态，但仍会尝试停止/刷新会话，并可能报告清理错误。处于 FAILED 状态的运行时
必须先反初始化/重新初始化，才能重新启动。

`stop()` 之后的 `start()` 会重建协议状态。已配置的节点会发送启动报文，并在 NMT
Pre-operational 状态等待主站；节点 ID 255 则保持 LSS 未配置状态。应用值、通信设置
和批量 ID 在停止/启动之间保留，但协议栈拥有的状态不会保留：例如错误寄存器、EMCY
历史/故障位和守护状态会被重新初始化。重新启动后，请重新声明仍然有效的模型故障。
协议栈设置会应用待定的 LSS 节点 ID 并重新计算相对 ID；它不会激活待定的波特率。
成功的停止会刷新网络队列，因此过期帧不会残留。NMT Stop 命令则不同：工作线程仍保持运行。

Reset Communication 恢复通信默认值，保留应用值和待定的 LSS 设置。Reset Node 恢复
所有 OD 默认值，并重新加载已保存/启动时的 LSS 设置。两者都会重建服务，而不改变
主机句柄或 OD 结构；批量访问在每次调用时解析当前的扩展。成功的协议复位会在
`get_status()` 中递增各自独立的计数器，因此外部模型无需从瞬时的 NMT 状态推断复位类型。

成功的反初始化会释放会话、OD 和协议栈，并使所有批量 ID 失效。同一句柄内 ID 不会
重复使用。诊断信息会一直保留，直到被显式清除或句柄被销毁。

每个句柄上的操作是串行化的；工作线程使用同一个递归 OD 互斥锁。持久化 I/O 会释放
OD 互斥锁，同时保留工作线程对协议处理的所有权。在 LSS 存储期间，调用方可以更新
通道，但拆除操作会先 join 工作线程，再释放其状态。

调用方必须协调 destroy 与其他所有调用，并且绝不能传入过期、伪造或已销毁的句柄。
destroy 失败时句柄会被保留，以便重试清理。空句柄返回参数错误。

调用方（例如 LabVIEW）必须保证接口和持久化路径的独占性，包括跨库卸载/重新加载周期。
库不会跟踪重复的接口或路径。NI-XNET 仍负责检测外部的冲突使用。

<a id="json-model-version-1"></a>

## JSON 模型版本 1

参见 [examples/device.json](examples/device.json)。这是一个显式的模拟器模型，
而不是任意 EDS 的直接序列化。

必需的根字段：

| 字段 | 含义 |
| --- | --- |
| `schema_version` | 值为 `1`，使用下文描述的无符号数语法 |
| `node_id` | `1..127`，或 `255` 表示 LSS 调试 |
| `bitrate_kbps` | 整数启动波特率，范围 `1` 到 `1000` kbit/s；必须被目标 XNET 接口支持 |
| `identity` | `vendor_id`、`product_code`、`revision_number`、`serial_number`，均为 uint32 |
| `objects` | 应用 VAR/ARRAY/RECORD 描述数组 |

可选的根字段：

| 字段 | 默认值 | 范围 / 含义 |
| --- | --- | --- |
| `device_type` | 0 | uint32，生成 `0x1000` |
| `heartbeat_ms` | 1000 | uint16 毫秒；0 表示禁用心跳生产 |
| `worker_period_us` | 1000 | `100..100000` 微秒，为处理之后工作线程的等待时间；不是固定的周期截止期限 |
| `rpdos`、`tpdos` | 空数组 | 每个方向最多配置 8 个槽位 |
| `node_guarding`、`sync`、`emcy` | 空配置对象 | 服务使用[服务配置](#node-guarding-sync-and-emcy)中的默认值，省略并不会禁用它们 |

厂商 ID 和序列号必须非零；请为物理网络上的每个设备使用合适的唯一标识。示例中的
标识仅作说明。

未知字段、无效类型和超出范围的默认值都是错误。输入文件限制为 4 MiB；自定义对象
限制为 4096 个。索引/子索引和无符号数值（包括无符号配置字段）可以使用非负 JSON
整数，或十进制/小写前缀 `0x` 的字符串。有符号应用值使用 JSON 整数；BOOLEAN 默认值
接受 JSON 布尔值或无符号值 0/1。REAL32/64 默认值必须是有限的 JSON 数，且适合所选
类型。生成 JSON 时请保持 64 位整数精确，不要经由浮点通道往返转换。

### 对象与值

自定义对象索引范围为 `0x2000..0xBFFF`。标准通信对象由构建器生成，不要在 `objects`
中重新定义：
`0x1000, 0x1001, 0x1003, 0x1005..0x1007, 0x100C..0x100E, 0x1014, 0x1015, 0x1017..0x1019,
0x1200`，以及已配置的 PDO 通信/映射记录。

- VAR：`index`、可选的 `kind: "var"`，以及下方的值字段。
- ARRAY：`index`、`kind: "array"`、不含 `value` 的公共值字段，以及
  `values: [...]`。最多 254 个元素；子索引 0 自动生成为只读的元素计数。
- RECORD：`index`、`kind: "record"` 和 `members: [...]`。每个成员都有一个显式的
  `subindex`（`0..254`）和值字段。子索引 0 必须是只读 UNSIGNED8，内容为最高子索引，
  且不具备本地写入/PDO 权限。

数组元素/记录字段分别寻址。数组在内部被降级为 CANopenNode RECORD 描述符，各元素
拥有独立对齐的值存储；其对外的 OD 索引/子索引语义仍与数组一致。布局和地址在
反初始化之前保持冻结。

值字段：

| 字段 | 含义 |
| --- | --- |
| `type` | `BOOLEAN`、`UNSIGNED8/16/32/64`、`INTEGER8/16/32/64`、`REAL32/64`、`VISIBLE_STRING`、`OCTET_STRING` |
| `value` | 必需的初始标量/字符串值；八位字节串使用字节数组 |
| `value_offset` | 可选 `"NODE_ID"`，用于整数默认值：恢复默认值时将当前节点 ID 加到 `value` 上 |
| `size` | 仅字符串需要：固定容量 `1..4096` 字节 |
| `access` | SDO 访问 `ro`、`wo` 或 `rw`（默认 `rw`） |
| `pdo` | `none`、`tx`、`rx` 或 `both`（默认 `none`） |
| `local_write` | 批量通道是否可以写入（默认 false），与 SDO 访问相互独立 |

VISIBLE_STRING 默认值使用可打印 ASCII，不包含内嵌 NUL，并在固定槽位中为 NUL 留出
空间。OCTET_STRING 默认值可以短于容量，剩余部分以零填充。本版本中字符串不可映射到
PDO。明确不支持 Unicode 字符串、DOMAIN、非标准整数宽度、可变大小的通道槽位以及
任意厂商专用通信对象。

`value_offset: "NODE_ID"` 支持有符号/无符号整数 VAR、除子索引 0 以外的记录成员，
以及对每个 ARRAY 元素统一应用的偏移。存储的 `value` 是基值，而不是已经解析的数值。
初始化和 Reset Node 会针对当前节点（包括持久化的启动设置）计算默认值。
Reset Communication、停止/启动以及 LSS 节点 ID 变更不会覆盖实时应用值。节点 ID 255
（未配置）贡献零。加法结果必须适合声明的类型；溢出是错误，而不是回绕运算。

### EDS 转换

在本目录下生成模板并捕获警告：

```powershell
python .\tools\eds_to_json.py .\eds\Kinco_FD_20250417_V1.2.eds .\examples\kinco.template.json 2> conversion-warnings.txt
```

输出参数是可选的。只提供 EDS 路径时，转换器会将其扩展名替换为 `.json`，并写在
源 EDS 旁边；`.inputs.md` 说明文件放在同一文件夹中。显式的输出路径仍然优先：

```powershell
python .\tools\eds_to_json.py .\eds\Kinco_FD_20250417_V1.2.eds
```

脚本仅使用 Python 标准库。`--encoding cp1252` 可显式选择默认 `utf-8-sig` 之外的
编码；编码错误不会被静默替换。`--warnings-as-errors` 会报告警告但不写入输出。EDS
解析/转换校验错误在写入任何输出之前发生。输出文件的写入本身是顺序的而非原子的：
I/O 失败可能留下被截断/替换的 JSON 文件，或导致 JSON 与说明文件不一致。请将已调试
完成的模型与转换器输出分开保存。

输出是一个**模板**，不一定是可直接初始化的模型。未知值是字面 JSON 字符串
`"{USER_INPUT}"`，包括节点 ID、所选波特率、工作线程周期、缺失的必需非零标识字段、
字符串容量、未知类型，以及无法解析的表达式或映射。初始化前请将每个占位符替换为
类型正确的 JSON 值：例如 `10`、`125` 或 `true`，而不是加引号的数字/布尔文本。
运行时会拒绝未解析的占位符并报告其 JSON 路径。字符串容量必须为可见字符串的结尾 NUL
留出空间。对于已知初始值的 `ro`/`const` 字符串，容量会自动推断：VISIBLE_STRING 为
ASCII 字节长度加一，OCTET_STRING 至少一个字节。可写字符串和默认值未解析的字符串
仍需要显式容量。

只读 SDO 访问不会免除运行时的存储需求，也不会在 `local_write: true` 时禁止主机写入。
推断出的容量是固定的：更长的主机字符串需要手动增大 `size`。对于 Kinco `0x6504`，
已知的制造商字符串会生成 `size: 31`，无需占位符。

为减少手动编辑，生成的应用值带有 `local_write: true`，与其网络 SDO 权限无关。
结构性子索引 0 保持只读，且不可本地写入。缺失的数值 OD 默认值变为 `0`；缺失的
VISIBLE_STRING/OCTET_STRING 默认值变为 `""`/`[]`。已有的 EDS 默认值会被保留。
缺失的受支持数值通信值也会变为零，但必需的非零标识值、COB-ID 和映射结构不会被猜测。
特别地，缺失的 PDO 传输类型会变为 `0`（同步非周期，等待 SYNC），**而不是**异步类型
254/255。自动默认值会作为警告报告：它们是模拟器的初始值，并不代表真实设备的出厂
设置，且可能与 EDS 限值不同。不受支持的表达式仍需手动解析，而不是置零。

不需要 JSON 注释：转换器还会在 `kinco.template.json` 旁边写入
`kinco.template.inputs.md`。这个 Markdown 配套说明**不会**被模拟器加载。它通过可
搜索的 OD 索引/子索引和字段名来标识应用字段，而不是通过在 JSON 数组中的位置。
JSON Pointer 路径仅作为工具的辅助参考保留。其中包含预期类型/范围、描述和示例。
在适用的情况下，还包含 EDS 中支持的波特率以及对象名称/地址/默认表达式。它还保留了
所有转换警告，并解释自动默认值策略。在 VS Code 的 Markdown 预览中打开它，可以看到
易读的输入表和详细字段说明；只有模型文件仍是 JSON。

例如，`/node_id` 被描述为整数 `1..127`，或 `255` 表示 LSS 调试，示例为 `10`。
字符串的 `size` 字段会说明其字节容量和 NUL 要求，并给出其初始值所需的最小值。
应用字段的标签形如 `index=0x2001, subindex=0x00, size`：在编辑器中搜索 `0x2001`，
然后编辑该对象的 `size`（或所指示记录成员的字段）。示例只是指导，不是已确认的调试
设置；请编辑模型文件，而不是说明文件。`--warnings-as-errors` 不会改变这两个文件。

转换器：

- 导入 `0x2000..0xBFFF` 中的应用 VAR 以及显式的 ARRAY/RECORD 子条目。数组以记录
  形式输出，以保留每个子条目的类型、权限和稀疏索引。子索引 0 会被规范化（并给出
  警告）为运行时的只读 UNSIGNED8 最高子索引约定。
- 使用 `0x1018` 默认值作为标识，厂商、产品和修订号可回退到 DeviceInfo。缺失或为零
  的序列号需要用户输入。将设备类型、心跳、守护、SYNC 和 EMCY 设置转换为各自专用的
  JSON 字段，而不是重新定义通信对象。
- 从 `0x1600..0x1607` / `0x1A00..0x1A07` 读取实际 PDO 映射（遵循当前子索引 0 计数），
  从 `0x1400..0x1407` / `0x1800..0x1807` 读取通信设置。保留槽位编号、禁用位和时间
  单位。校验引用、方向、完整目标宽度以及 64 位负载限制。缺失的有效映射字会为整个
  映射列表生成占位符；已知无效的映射是错误。
- 独立于实际映射保留 PDO 可映射性。`ro`/`const` 允许 TX，`wo` 允许 RX，`rw` 允许
  两者，`rwr`/`rww` 分别选择 TX/RX。在此运行时中 BOOLEAN 占一个字节；不支持位级/
  部分字段映射。
- 识别整数常量、`$NODEID`、`$NODEID + constant` 和 `constant + $NODEID`，不会求值
  任意 Python 或 EDS 表达式。整数应用默认值使用 `value_offset`；PDO/SYNC/EMCY ID 使用
  `cob_id_offset`。其他表达式，或位于不支持偏移的字段中的表达式，会生成显式占位符
  和带源文本的警告。

例如，PDO 通信 COB-ID 中的 `DefaultValue=$NODEID+0x240` 会变为：

```json
{
  "cob_id": 576,
  "cob_id_offset": "NODE_ID"
}
```

运行时在节点 10 时将其解析为 `0x24A`，在节点 42 时解析为 `0x26A`。标量默认值
`$NODEID+0x80` 会变为 `"value": 128, "value_offset": "NODE_ID"`；应用默认值遵循上述
复位规则，而不是 COB-ID 的实时重新计算。这些可选的 schema 版本 1 扩展需要更新后的
运行时；旧版运行时二进制会拒绝这些新键。

每条警告都会打印到 stderr 并保存在配套说明中。警告涵盖缺失输入、自动默认值、
规范化的计数、不支持/未表示的字段、未使用的元数据和定义、省略的通信对象、
固定/只读 PDO 记录、RTR 限制，以及将使用运行时默认值的缺失可选服务。名称、限值、
标志和支持功能列表不会作为运行时校验规则导入。不受支持的数据类型保留为显式的
类型/值占位符；紧凑子对象布局和不受支持的对象类型需要手动展开或转换，并会报错。
不会合成任何设备行为。

独立的转换器测试：

```powershell
python -m unittest discover -s .\tests -p test_eds_to_json.py
```

当找到 Python 3.10+ 时，CMake 测试构建还会注册转换器测试，并通过真实的 C++ 加载器
校验已填充的测试模板。客户 EDS 检查仅在相应的提供文件存在时运行。端到端测试会为
剩余占位符提供测试设置；生产调试设置留给用户。

### PDO

每个 `rpdos`/`tpdos` 数组按 PDO 编号顺序最多支持 8 个条目。数组位置定义 PDO 编号，
包括非活动槽位。生成的通信/映射记录覆盖 RPDO 的 `0x1400..0x1407` / `0x1600..0x1607`
以及 TPDO 的 `0x1800..0x1807` / `0x1A00..0x1A07`（按配置）。每个条目包含：

- `mapping`：0..8 个对象 `{ "index": ..., "subindex": ... }`；总负载 <=8 字节。
- `enabled`：可选布尔值，非空映射默认 true，空映射默认 false。启用的 PDO 需要非空
  映射。设为 false 可保留非空映射而不激活该 PDO。
- `transmission_type`：0..240 或 254/255，默认 255。
- `event_ms`：uint16，默认 0；TPDO 事件时间或 RPDO 超时。
- `inhibit_100us`：uint16，默认 0，仅用于 TPDO；RPDO 请省略此字段，构建器不会使用
  或校验其值。
- `cob_id`：显式的标准 11 位 CAN 标识符，不含控制位。PDO1..4 可选：省略时，每次
  协议栈复位都会为当前节点解析默认连接集 ID。PDO5..8 必需，包括非活动槽位。除非
  指定 `cob_id_offset`，PDO5..8 的 ID 是绝对值，不会加上节点 ID。
- `cob_id_offset`：可选 `"NODE_ID"`，需配合显式 `cob_id`。将该 ID 视为基值，在初始化
  和通信复位时加上当前节点 ID，包括 PDO5..8。节点 255 贡献零。省略时，显式 ID 保持
  现有的绝对 ID 行为。解析后的 ID 必须是有效、不受限的 11 位 ID，且不得与其他本地
  服务冲突。

禁用的 PDO 在其生成的通信 COB-ID 中设置 bit 31，既不发送也不应用接收到的数据。
空映射是有效的非活动槽位，而不是映射错误。`request_tpdo()` 会拒绝非活动槽位。
它们配置的 ID 仍会保留用于冲突检查，与禁用的 EMCY ID 一致。使能状态和映射在
反初始化/重新初始化之前保持固定。

例如，额外的 TPDO 槽位可以使用
`{"cob_id": "0x43F", "mapping": [{"index": "0x2000"}]}`；未使用的槽位可以使用
`{"cob_id": "0x43E", "mapping": []}`。将它们放在预期的数组位置，并配置主站与之匹配。
不要将 PDO1..4 的默认 ID 公式扩展到 PDO5..8：它会与 SDO/其他服务 ID 重叠，并最终
超出 11 位。

映射必须具有正确的 `pdo` 方向属性。本地 CAN-ID 冲突和受限的显式 ID 会被拒绝。
固定的 PDO 通信/映射记录对 SDO 只读；动态 PDO 重映射保持禁用。最终的解析后 ID
冲突检查针对已配置节点运行；节点 ID 为 255 时，检查会推迟到应用已配置的节点 ID 时。
可写的服务通信对象见下文。调用方还必须避免与总线上其他设备冲突。每个方向 8 个槽位
并不意味着 16 个 PDO 都能在每个工作周期发送；请根据物理 CAN 总线容量规划发送周期和
事件流量。

<a id="node-guarding-sync-and-emcy"></a>

### 节点守护、SYNC 与 EMCY

这些服务复用 CANopenNode 的实现，OD 回调在初始化时安装，并在通信/节点复位时重建。

| 可选 JSON 对象 | 字段与默认值 | OD / 行为 |
| --- | --- | --- |
| `node_guarding` | `guard_time_ms`：uint16，默认 1000；`life_time_factor`：uint8，默认 3 | SDO 可写的 `0x100C`/`0x100D`。寿命时间为二者之积（毫秒）。乘积以微秒计超出 uint32 时被拒绝，包括运行时 SDO 写入。 |
| `sync` | `cob_id`：11 位 ID，默认 `0x80`；`period_us`：uint32，默认 0；`window_us`：uint32，默认 0；`counter_overflow`：0 或 2..240，默认 0 | SDO 可写的 `0x1005`/`0x1006`/`0x1007`。协议栈消费 SYNC，在周期非零时监视其周期，并应用配置的同步窗口。`0x1019` 计数器格式为只读，在初始化时确定。不启用 SYNC 生产。 |
| `emcy` | 可选 `cob_id`：显式 11 位 ID，可带禁用位 `0x80000000`；省略表示 `0x80 + 当前节点 ID`。`inhibit_100us`：uint16，默认 0 | SDO 可写的 `0x1014`/`0x1015`，使用协议栈的可配置生产者和禁止时间计时器。 |

当 `heartbeat_ms > 0` 时，心跳生产优先，节点守护被禁用。设置 `heartbeat_ms: 0`
（或向 `0x1017` 写入零）以使用守护。从站响应 `0x700 + 节点 ID` 上的 RTR 请求，
回复 NMT 状态和交替翻转的 bit 7。`0x100E` 以只读方式暴露该标准 ID；不支持自定义
守护 ID。第一个请求会启动寿命守护；将守护时间或寿命因子设为零会禁用超时监视，
但不会禁用 RTR 回复。重新启用心跳或禁用监视会清除待处理的守护超时。封装层现有的
NMT 错误策略会在寿命守护超时时将 Operational 节点切换到 Pre-operational。清除超时
不会自动返回 Operational；该转换由主站控制。心跳消费者和节点守护主站保持禁用。

<a id="deferred-communication-interruption-policy"></a>

#### 延后的通信中断策略

当前范围不要求可配置的中断响应。`get_status().node_guarding_timeout` 向外部模型
报告当前状况，驱动器报警和运动响应由外部模型负责。然而，通信协议栈会独立地按其
EMCY 规则报告守护错误，并将 Operational 节点切换到 Pre-operational，阻止
Operational 下的 PDO 交换。模型仅仅忽略该标志无法撤销这些影响。

已配置的应用对象（例如 Kinco `0x6007:00`）目前只是普通的 OD 存储；其值不会改变本库
的策略。如果之后的测试需要"忽略中断"设置，请先确认预期的 NMT、PDO、EMCY 和恢复
行为。然后提供通用的通信错误策略机制，由设备适配器/模型负责解释厂商对象。这是
一个重要的延后兼容性事项，不是已实现的选项，也不是当前需求。现有的超时检测和报告
保持不变。

基于守护的模拟的可选字段示例（合并到完整的版本 1 模型中）：

```json
{
  "heartbeat_ms": 0,
  "node_guarding": { "guard_time_ms": 1000, "life_time_factor": 3 },
  "sync": { "cob_id": "0x80", "period_us": 0, "window_us": 0 },
  "emcy": { "inhibit_100us": 0 }
}
```

通过 SDO 修改 SYNC ID 会立即更新接收过滤器。修改已启用 EMCY 生产者的 ID 时，需要
先通过 bit 31 禁用它，然后选择/启用新 ID。受限 ID、本地 SYNC/EMCY/PDO ID 冲突以及
不支持的 COB-ID 位会被拒绝。禁用的 EMCY ID 仍保留用于冲突检查。显式 EMCY ID `0x80`
会被拒绝，因为协议栈将该值视为相对节点的默认值，而不是绝对 ID。

对这些设置的 SDO 修改在停止/启动之间保留。Reset Communication 和 Reset Node 会恢复
JSON 通信默认值；普通的通道/SDO 修改不会被持久化。省略这些可选对象时，现有的
心跳/PDO 默认值保持不变。工作线程/XNET 调度仍不提供严格的 SYNC 时序保证。

SYNC 和 EMCY 也接受与显式 `cob_id` 基值配合的 `cob_id_offset: "NODE_ID"`。它们遵循
与 PDO 相同的相对节点重新计算和解析后 ID 校验。被接受的 SDO 修改会在停止/启动之间
覆盖该相对启动默认值；通信/节点复位会恢复并重新计算它。EMCY 省略 `cob_id` 时仍选择
现有的 `0x80 + 节点 ID` 默认值。

<a id="batch-abi"></a>

## 批量 ABI

绑定使用三个数值数组：uint16 索引、uint8 子索引和 uint32 大小。每个数组有 `count`
个元素。在 LabVIEW 中，将它们配置为以 **Array Data Pointer** 传递的数值数组，而不是
数组句柄或簇数组。调用前请校验数组长度：C 接收的是指针，而不是数组维度。
`get_status()` 描述符包含十个 uint32 字段（40 字节）；它与批量操作返回的逐条目
int32 状态数组是分开的。

`bind_batch()` 复制这些数组，并校验精确的 OD 长度、OD 地址互不相同、绑定数 <=4096
以及总大小 <=1 MiB。值按数组顺序占用连续的字节槽位，没有填充：offset[i] 是 i 之前
所有大小之和。偏移在内部计算，而不是由调用方提供。为了获得最低的运行时延迟，请在
启动前配置好组。

```c
uint16_t indices[] = {0x2000, 0x2001};
uint8_t subindices[] = {0, 0};
uint32_t sizes[] = {2, 4};
uint32_t batch;
int32_t result = xnet_canopen_bind_batch(
    handle, indices, subindices, sizes, 2, &batch);
/* Check result before using batch. */
uint8_t buffer[6] = {0x34, 0x12, 0, 0, 0, 0};
int32_t entry_status[2];
result = xnet_canopen_write_batch(
    handle, batch, buffer, sizeof(buffer), entry_status, 2);
```

所有数值均使用小端编码；REAL32/64 使用 IEEE-754。字符串使用固定槽位，批量写入
VISIBLE_STRING 时必须包含 NUL。缓冲区/数组由调用方拥有，库不会保留。`buffer_size`
是字节容量，至少为所绑定大小之和（示例中为 6）。`entry_status` 是调用方分配的 int32
输出数组：元素 i 报告绑定变量 i 的结果。`entry_count` 是其元素个数而非字节数，且必须
等于绑定变量的个数（示例中为 2）。在 LabVIEW 中，将字节缓冲区和状态数组作为数值
Array Data Pointer 传递。每次写调用都会尝试所有绑定条目。由 LabVIEW/VeriStand 决定
何时调用；没有使能掩码。当变量必须独立写入时，请绑定单独的组或单条目批次，而不是
重写可能已在网络上改变的无关值。

调用级错误不会改变条目状态，并在任何写入之前被拒绝。单个条目的失败按条目记录；
后续条目继续执行，之前成功的写入不会回滚。只要有任一条目失败，总体结果即为
`XNET_CO_PARTIAL_FAILURE`。失败的读取槽位不得作为有效数据使用。本地写入需要
`local_write`；读取需要 SDO 可读的元数据。绑定会调用当前的 OD 回调，而不是直接写
原始内存。工作线程和通道写入是串行化的：最后一次成功的写入生效。批次可以防止与
工作线程交错，但不是可回滚的事务。

基于早期描述符数组/写使能 API 构建的调用方必须重新构建，并更新其 LabVIEW Call
Library Function Node。当前 API 既没有写使能参数，也没有 `XNET_CO_SKIPPED`。

<a id="tpdo-faults-and-errors"></a>

## TPDO、故障与错误

`request_tpdo(handle, one_based_number)` 要求节点处于运行中且为 Operational 状态。
在 LSS 波特率切换期间的请求会被拒绝。类型 254/255 遵守禁止时间；类型 0 等待 SYNC。
周期同步类型 1..240 拒绝手动请求。请求可能被合并，并发送当前值。请求被接受并不代表
总线发送确认。通道写入不会自动请求 TPDO；写入和请求是两个独立的操作。

`set_fault()` 报告/复位位 `0x40..0x4F` 上的模拟制造商故障。它要求节点处于运行中且
已完成 LSS 配置，但不要求 NMT Operational。`active` 必须为 0 或 1；声明故障时请使用
非零的 CANopen 错误码。协议栈根据其 NMT 和生产者规则生成 EMCY 并更新错误状态/历史；
主机 API 失败不会自动发出 EMCY。

每个操作都会返回公共头文件中定义的状态。`get_status()` 暴露运行时/NMT 状态、当前/
待定的 LSS 值、独立的复位完成计数器以及 CAN 错误标志，另外还有
`node_guarding_timeout`（0 或 1）。该超时会在收到新的守护请求、禁用监视、启用心跳
或协议栈重启/复位时清除。它是当前状况，而非事件历史。外部模型决定驱动器的响应
（例如依据 `0x6007`）；此处不添加 CiA 402 状态机。协议栈根据其 NMT/生产者规则报告/
清除守护通信错误并发出 EMCY。

<a id="status-snapshot-and-reset-notification"></a>

### 状态快照与复位通知

`get_status()` 仍是唯一的只读运行时/协议信息 API。它不需要批量绑定、JSON 配置，
也不需要额外的网络可见 OD 条目。所有字段在同一个运行时锁下复制。

| 字节偏移 | uint32 字段 | 含义 |
| --- | --- | --- |
| 0 | `runtime_state` | 主机库生命周期状态。 |
| 4 | `nmt_state` | 当前 CANopen NMT 状态，不是 CiA 402 驱动器状态。 |
| 8 | `active_node_id` | 当前节点 ID。 |
| 12 | `pending_node_id` | LSS 选定的节点 ID。 |
| 16 | `active_bitrate_kbps` | 已应用的接口波特率。 |
| 20 | `pending_bitrate_kbps` | LSS 选定的波特率。 |
| 24 | `can_error_status` | 协议栈 CAN 错误位掩码。 |
| 28 | `node_guarding_timeout` | 当前寿命守护超时状况，0 或 1。 |
| 32 | `reset_node_count` | 成功完成的 Reset Node 操作次数。 |
| 36 | `reset_communication_count` | 成功完成的 Reset Communication 操作次数。 |

**ABI 变更：** 该结构现在为 40 字节，之前为 32 字节。原有八个字段保持其偏移；
`node_guarding_timeout` 仍位于偏移 28。在使用更新后的库之前，请重新构建 C 调用方，
并将 LabVIEW Call Library Function Node 的输出描述符扩展为十个 uint32 字段。该函数
没有输出大小参数，无法安全地接受旧的 32 字节缓冲区。

复位计数器存储在 OD 和协议栈之外。只有在复位恢复了所需的 OD 值、重建了服务并成功
重启会话之后，相应计数器才会递增。这表示本地复位已完成，而不是确认启动报文已到达
总线。Reset Node 只递增其自身的计数器；其通信重新初始化不会同时递增通信计数器。
协议栈请求的协议复位走同一路径，包括 LSS 调试请求通信复位的情况。

两个计数器均从零开始。成功的 `initialize()` 会清零它们；失败的初始化尝试不会。
它们在协议复位、主机停止/启动、反初始化和运行时故障之后都保留。没有协议栈时，保留的
计数器和运行时状态仍可读取，其他字段为零。轮询和 `clear_error()` 都不会清除它们，
普通的初始化/启动/停止也不计为协议复位。计数器不会跨已销毁的句柄持久化。

模型启动时，记录一个基线。每个模型步骤都将两个计数器与上一次快照比较：Reset Node
通常要求以与应用 OD 默认值一致的方式重新初始化外部设备/被控对象模型；Reset
Communication 保留应用状态。库不会复位外部模型，也不实现 CiA 402 行为。每个计数器
按 2^32 取模回绕；uint32 减法得到的是取模 2^32 后的完成次数。独立计数器能在两次读取
之间同时保留两种复位类型，但不保留它们的先后顺序。两次读取之间完整回绕一圈的情况
与没有变化无法区分。

状态和 OD 批量调用是分别同步的。为了获得一致的只读模型输入采样，请先读取状态，
再读取输入批次，然后再次读取状态。如果任一计数器发生变化，则丢弃/重试该输入采样
并处理复位。这不是围绕模型执行/反馈写入的事务：在最后一次检查与反馈写入之间仍可能
发生复位。还要检查运行时和批量结果，尤其是在复位失败之后。当前 NMT 状态和守护
超时是快照，而不是无损的状态转换历史。

硬件总线关闭（bus-off）/错误计数器会反映到协议栈的 CAN 错误状态中。溢出标志会一直
锁存到协议栈重启/复位，以使数据丢失保持可见。

操作结果使用 `XNET_CO_*`（`XNET_CO_OK`、`XNET_CO_INVALID_STATE` 等）。独立的
`runtime_state` 字段使用 `XNET_CO_RUNTIME_UNINITIALIZED`、`XNET_CO_RUNTIME_STOPPED`、
`XNET_CO_RUNTIME_RUNNING` 和 `XNET_CO_RUNTIME_FAILED`。这些运行时名称取代了最初的
`XNET_CO_UNINITIALIZED/STOPPED/RUNNING/FAILED` 名称；其数值（0..3）不变。运行时状态
和 API 结果都不是 CANopen NMT 状态或 CAN 错误位掩码。公共头文件记录了每个枚举值、
结构字段和 API 契约。

`get_last_error()` 以原子方式复制每个句柄最近一次失败的代码/消息快照。
`message_required` 包含 NUL；容量不足时返回 `XNET_CO_BUFFER_TOO_SMALL`，且不会替换
原始诊断。使用 NULL 消息/容量 0 查询可获得所需大小。消息上限为 2047 字节。成功的
操作和诊断读取不会清除错误。`clear_error()` 清除的是诊断，而不是物理总线故障或锁存
标志。

工作线程的致命错误会使运行时进入 FAILED 并使会话静默。请反初始化后重新初始化以
恢复；不要因为之前启动成功就假定工作线程仍然健康。清理失败会被明确报告且可重试。
非零 XNET 状态（包括警告）会产生诊断，而不会被静默接受。并非所有诊断都会停止通信：
输出队列溢出会保留待发送帧以便重试，输入队列溢出会刷新未读输入并记录丢失。不受支持
的入站帧类型/ID/负载会被忽略并产生诊断。除了最近的错误，还要检查运行时状态。

<a id="file-logging"></a>

## 文件日志

在 [src/logging_config.h](src/logging_config.h) 中将 **`XNET_CANOPEN_LOG_PATH`** 设置
为所需的绝对文件路径，然后重新构建。同名的编译器定义会覆盖头文件中的设置。
刻意不提供运行时路径参数或 JSON 日志选项。默认路径为：

- NI Linux RT/Linux：`/var/local/natinst/log/xnet_canopen.log`
- Windows：`C:\ProgramData\xnet_canopen\xnet_canopen.log`

部署账户必须能够创建/写入父目录。spdlog sink 会创建缺失的父目录。日志在
`initialize()` 期间、获取 XNET 会话之前首次打开。打开/初始化文件失败会返回
`XNET_CO_IO_ERROR`，并可通过 `get_last_error()` 获取。句柄分配本身不会打开文件；
日志启动之前的错误仍可通过常规 API 诊断获取。

本库实例中的所有句柄共享一个线程安全的滚动 sink。活动文件始终是宏定义的路径，
滚动阈值为 5 MiB，最多保留三个备份（例如 `xnet_canopen.1.log`）。已有日志会被追加
而不是截断。不要让其他进程/库实例同时滚动或写入同一文件。本集成不会改变宿主应用的
默认 spdlog logger 或注册表。

每条设备记录包括：

```text
[timestamp] [pid=... tid=...] [info] [device=7 interface=CAN1 identity=00000300:00004644:00000000:000003e9 node=10 kbps=500] device started
```

**主要追踪键是进程 + 设备实例编号 + 完整标识**（`vendor:product:revision:serial`，
十六进制）。实例编号在每个句柄上分配一次，并在停止/启动、NMT 复位和 LSS 变更之后
保持不变。接口、当前节点 ID 和波特率是上下文信息，而不是稳定的键：仅凭节点 ID 并不
合适，因为它可能改变，或在另一条总线上被复用。节点 `255` 表示 LSS 未配置；节点 `0`/
零标识表示启动/清理上下文不可用。同一句柄上的新一次初始化可能加载不同的标识。

记录的事件包括生命周期和 NMT 变化、NMT 复位、被接受的通信设置写入、LSS 待定设置/
波特率激活/存储、守护超时/恢复、模型故障声明/清除以及 API/工作线程错误。连续相同的
API/驱动诊断会被去重，直到存储的诊断发生变化或被清除。正常的批量值和原始 CAN 帧
不会被记录，因此普通的周期运行不会产生逐帧日志流量。日志中观察到的状态变化不能替代
无损的应用事件 API。

日志是同步的，每条记录发出后都会刷新用户空间缓冲区；但不承诺掉电持久性。磁盘 I/O
可能延长 API/工作线程的执行时间，这与当前非严格时序的范围一致。内部设备日志调用接受
spdlog 风格的 `{}` 格式参数；纯消息按字面记录。格式化失败与 sink 失败走同一诊断路径，
其固定容量的错误报告不会分配内存。如果之后的写入/滚动/刷新失败，集成会将其报告到
stderr 和句柄诊断，而不会停止 CAN 通信或撤销已完成的协议操作。如果此前没有活动诊断，
则记录 `XNET_CO_IO_ERROR`；否则保留原始代码，并在空间允许时追加日志失败文本。成功的
API 调用不会清除该诊断。

当 `XNET_CANOPEN_BUILD_TESTS=ON` 且 `XNET_CANOPEN_BUILD_LIBRARY=OFF` 时，CMake 会将
集成日志路径覆盖为构建目录中的 `xnet_canopen-test.log`。在构建共享库的同时启用测试
不会覆盖其部署日志路径。独立的日志测试可执行文件始终使用其自身的构建目录路径。测试
覆盖了节点 ID 变更而不丢失追踪键、并发写入、滚动阈值、启动时打开失败/重试，以及
运行时 sink 失败而无递归日志或原始错误丢失。

<a id="lss-and-persistent-configuration"></a>

## LSS 与持久化配置

现有的 LSS 从站提供全局/选择性选择、fastscan、标识和节点 ID 查询、节点 ID 配置、
波特率配置/激活以及 Store Configuration。封装层实现适配器和存储回调。LSS 使用标准
固定速率表：`10, 20, 50, 125, 250, 500, 800, 1000` kbit/s。启动 JSON 和已保存的配置
也接受 `1..1000` kbit/s 范围内的其他整数速率；它们不受 LSS 表限制。应用速率时，XNET
会报告不受支持的硬件设置。自动波特率检测（表索引 9，以速率 `0` 传给回调）会被显式
拒绝，因为没有提供检测实现。

激活过程会立即停止/刷新会话，等待请求的延迟，修改接口波特率，再次等待该延迟，然后
恢复。在这些间隔内，协议处理/发送被挂起。适配器失败会将工作线程标记为 FAILED；
激活不会伪造成功响应。

存储操作写入一个恰好包含四个字段的 JSON 对象：`schema_version: 1`、
`identity`（形如 `[vendor_id, product_code, revision_number, serial_number]`）、
`node_id` 和 `bitrate_kbps`，使用待定的 LSS 设置。该标识表示方式与设备模型中的命名
对象不同。文件缺失或加载时发生任何异常，都会使用模型的两个启动设置。无效、不可读
或标识不匹配的文件不会导致初始化或 Reset Node 失败；加载异常会作为
`XNET_CO_IO_ERROR` 记录在诊断中。已保存的值只覆盖启动时的 LSS 设置。加载发生在初始化
和 Reset Node 时，而不在 Reset Communication 或停止/启动时。普通的通道写入和临时 LSS
变更不会写盘。

在 Linux 上，替换过程在目标目录中使用独占的临时文件，完整写入 + fsync，原子重命名，
再对目录 fsync。只有在完成之后才返回 LSS 存储成功。目录同步失败可能发生在替换之后，
因此报告的存储失败并不能证明旧文件仍然存在；请检查/重试。正常的 1000 Hz 路径中不会
发生闪存写入。持久化路径必须包含父目录部分，且该目录必须已经存在；初始化不会预检
可写性。OD 存储/恢复对象 `0x1010/0x1011` 不由此 LSS 机制实现。

<a id="external-device-model-integration"></a>

## 外部设备模型集成

现有的批量、状态、TPDO 请求和故障 API 就是与外部 CiA 402 或其他设备模型之间的边界。
普通的命令/反馈交换不需要专门的驱动器配置文件 API。只生成约定模式/测试所需的对象和
通道，而不是 EDS 中的每个对象。对于每个通道，记录索引/子索引、类型、字节大小、
有无符号、原始单位/缩放、所有权、默认值和复位行为。

### 所有权与权限

| 所有者 | 典型数据 | 推荐策略 |
| --- | --- | --- |
| 主站 | 命令和网络配置的参数 | 使用设备所需的网络权限；通常 `local_write: false`。绝不要将缓存的命令周期性回写到 OD。 |
| 外部模型 | 反馈、状态字和应用报警值 | 通常 SDO `ro` 且 `local_write: true`；通过反馈写批次更新。 |
| 协议栈 | NMT 状态、通信计时器、错误寄存器和协议状态 | 使用受支持的服务/状态接口，而不是由模型任意写入底层内存。 |

EDS 转换器有意为应用值输出 `local_write: true`，以简化准备工作。部署前请按选定的
所有权契约收紧这些生成的权限。绑定只校验存在性和大小；读/写权限失败会在使用批次时
按条目报告。批量读取目前要求 SDO 可读的元数据：对于真正网络只写的对象，没有独立的
本地读权限。

将命令读批次与反馈写批次分开。转换为 VeriStand 通道时，保持有无符号属性和精确的
64 位值。物理传感器、电压以及被控对象/负载属性可以直接进入外部模型；只有当真实设备
在 OD 中暴露它们时，才经由 OD 路由。

如果建模的 I/O 可以覆盖网络控制字/模式，请明确定义优先级和本地写权限，而不是盲目地
重写缓存的命令。

### 执行与命令可观测性

一个普通的功能循环如下：

```text
Initialize from provisioned JSON and bind command/feedback batches
Record reset-counter baselines
At each agreed VeriStand step:
    Read status, command/parameter batch, then status again
    Check operation and per-entry results; handle reset-counter changes
    Execute the external device/plant model using the host elapsed time
    Write feedback and check all per-entry results
    Request affected event-driven TPDOs after successful required writes
    Report fault assertion/clear transitions through set_fault()
```

请遵循[复位采样规则](#status-snapshot-and-reset-notification)；独立的 API 调用并不构成
围绕模型执行的事务。工作线程不会在 RPDO 应用与 TPDO 打包之间运行外部模型，因此该
循环不保证同一 SYNC 周期内的反馈。

当前范围是最新值轮询，命令节奏/握手需与主站约定。多个 RPDO 可能在应用之前覆盖待处理
数据，多个 SDO 写入也可能发生在两次模型读取之间。一次成功的读取证明的是 OD 访问，
而不是数据新鲜度，也不是观察到了每个控制字边沿/重复写入。NMT/守护状态和日志记录是
快照/观察，而不是无损的状态转换历史。单独的序列计数器可以检测缺失的写入，但无法保留
被覆盖的值。如果未来的功能需要每条命令，请在合并之前捕获，或定义合适的确认协议。

写入报警/状态字对象不会发出 EMCY。`set_fault()` 不会写入这些模型拥有的对象。请显式
保持内部报警字、驱动器错误码、状态位和 CANopen EMCY 表示之间的一致性。报告的是故障
转换，而不是无条件的重复声明。如果 TPDO/故障请求跨越 VeriStand 通道，请使用显式的
确认/序列方案，以避免意外重复操作。

### 扩展边界（非当前需求）

以下是有条件的能力限制，而不是必须实现的待办事项。仅当选定的设备/测试需要时，才
添加通用机制。

| 所需行为（如被选定） | 为何仅靠最新值批次不够 | 扩展方向 |
| --- | --- | --- |
| 每个命令边沿或重复的命令写入 | 值可能在 RPDO 缓冲区中或两次主机读取之间被合并。 | 在丢失之前捕获有序事件，或约定命令节奏/确认。 |
| 应用专用的 SDO 校验/中止 | 到模型轮询时，SDO 成功响应可能已经发出。 | 通过库内 OD 扩展同步校验；模型之后的决定无法追溯拒绝已确认的写入。 |
| 设备保存/默认命令 | LSS 存储只覆盖节点 ID/波特率；存储的命令字节没有持久化/复位副作用。 | 由设备适配器定义参数组和算法；如需要，提供可复用的命令/持久化钩子。 |
| 由厂商对象触发的波特率变更 | 当前物理激活通过 LSS 完成，而不是任意应用 OD 值。 | 由设备适配器解释厂商编码；如添加，使用受控的通用重配置路径。 |
| 可配置的通信错误响应 | 模型报警无法撤销协议栈的 NMT/PDO/EMCY 影响。 | 定义通用策略和恢复语义；将厂商对象解释保留在核心之外。 |
| 真正网络只写的模型输入 | 批量读取要求 SDO 可读的元数据。 | 仅在确认需要时添加独立的本地读语义；不要以扩大网络权限作为变通方案。 |
| 模型与单个 SYNC 协同 | 未导出面向模型的 SYNC 序列/事件/时间戳。 | 若插补需要，定义观察/锁存语义；运动插补保留在模型中。 |

<a id="kinco-configuration-and-compatibility-notes"></a>

## Kinco 配置与兼容性说明

这些说明保留了在配置或扩展模拟器时有用的设备专用发现。它们并不表示封装层再现了
完整的物理 Kinco 驱动器，也不表示列出的所有选项都是必需的。

### 来源与解读

- 主要行为参考：提供的
  [Kinco 驱动器手册](customer_input/Kinco%20低压伺服驱动器使用手册20260909.pdf)，
  特别是第 4、5、8、11 章和换算附录。
- 提供的配置参考：
  [Kinco EDS](eds/Kinco_FD_20250417_V1.2.eds)。
- 配置产物：[模板 JSON](examples/kinco.template.json) 及其
  [生成的输入说明](examples/kinco.template.inputs.md)。

下文中的手册页码指 PDF 物理页码（阅读器中从 1 开始的页码），而不是印刷的页脚；
正文中印刷页码比物理页码小一。转换后的文本、GUI 标签、简略的概览表和孤立的示例
不能替代详细的对象定义和实际的总线行为。手册本身包含相互矛盾/编辑性的内容。请以
CANopen 章节中的 FD1x3 从站为基准；其他产品系列和 EtherCAT/CoE 表格不会自动确立
CANopen 需求。请依据手册和所选固件确定设备行为；使用提供的 EDS 进行配置，并对照
CANopen 规范和约定的设备契约检查真正的冲突。

当前范围使用固定 PDO 以及按约定节奏的最新值命令轮询。厂商参数保存/默认行为、
厂商对象波特率激活、可配置的中断策略、自定义 SDO 校验和无损命令队列均为延后事项，
不因存储其 OD 对象而被隐含实现。

### 配置与固定 PDO 布局

当前检入的 Kinco 模板需要四个调试输入：`node_id`、`bitrate_kbps`、`worker_period_us`
和 `identity.serial_number`。生成的说明给出了范围以及 EDS 支持的波特率
`50, 125, 250, 500, 1000` kbit/s。这些是配置参考速率，不是对库启动范围或标准 LSS
表的限制。请选择唯一的标识/节点设置，替换每个占位符，并检查自动默认值和权限。
重新生成可能改变所需的输入列表。

EDS 声明了 8 个 RPDO 和 8 个 TPDO。其非空默认映射为：

| PDO / 映射记录 | 按 EDS 顺序的字段 | 字节数 |
| --- | --- | --- |
| RPDO1 / `0x1600` | `0x607A:00` I32 目标位置；`0x6060:00` I8 模式；`0x6040:00` U16 控制字 | 7 |
| RPDO2 / `0x1601` | `0x6081:00` U32 轮廓速度；`0x60FF:00` I32 目标速度 | 8 |
| TPDO1 / `0x1A00` | `0x6041:00` U16 状态字；`0x6063:00` I32 实际位置；`0x6061:00` I8 当前模式 | 7 |

其他 EDS 槽位的默认映射条目数为零，成为非活动槽位。请保留槽位编号和约定的负载顺序。
某些手册示例使用不同的顺序或不同的 TPDO 字段；它们是配置示例，而不是唯一通用的
布局。请将主站和 JSON 配置为完全匹配。PDO5..8 即使非活动也需要显式 ID；手册中额外
PDO 示例的 `0x43F` 是普通的 11 位 ID，并不说明需要扩展帧。

### 应用通道选择

此处的方向是相对于外部模型而言的。这是一份选择指南，而不是强制的完整 OD。通过批量
读取读取命令，通过具有相应 `local_write` 权限的批次写入反馈。

| 模型输入 | OD 地址 / 表示 | 维护注意事项 |
| --- | --- | --- |
| 控制字 / 请求的模式 | `0x6040:00` U16 / `0x6060:00` I8 | 驱动器状态命令、故障复位边沿和模式选择属于模型。 |
| 位置 / 速度目标 | `0x607A:00` I32 / `0x60FF:00` I32 | 详细的手册表格将其标为读/写/可映射；概览中的 `W` 简写并不构成已确认的本地读限制。 |
| 轮廓速度与加减速 | `0x6081:00`、`0x6083:00`、`0x6084:00` U32 | 详细表格中轮廓速度同样为读/写/可映射；使用原始缩放值。 |
| 转矩/电流命令与限值 | `0x6071:00` I16、`0x60F6:08` I16、`0x6073:00` U16 | 为该模式选择实际的命令路径/缩放；不要在传输代码中重新解释。 |
| 回零 | `0x6098:00`、`0x6099:01/02`、`0x609A:00` U32、`0x607C:00` I32、`0x6099:05` U8 | 按需选择回零方式、速度、加速度、偏移和偏移模式（手册第 103 页）。 |
| 停止行为 | `0x605A..0x605E:00` I16、`0x6085:00` U32 | 对停止选项码和快速停止减速度建模（第 107/108 页）。 |
| 插补 | `0x3011:01/02` 以及已确认的目标对象 | 对于 CANopen 模式 7，周期编码 0/1/2/3 分别表示 1/2/4/8 ms；时钟使能为 0/1。这并未规定目标缓冲/锁存契约。 |
| 超时响应、限值和 I/O 配置 | `0x6007:00` 以及选定的制造商对象 | 定义模型响应和 I/O 优先级；`0x6007` 不控制库的通信策略。 |

| 模型输出 | OD 地址 / 表示 | 维护注意事项 |
| --- | --- | --- |
| 状态字 / 当前模式 | `0x6041:00` U16 / `0x6061:00` I8 | 由模型而非库生成状态/模式位和被接受的模式。 |
| 实际位置 | `0x6063:00` I32 | 用于 CANopen 示例；未确认语义前不要替换为 EtherCAT 的 `0x6064`。 |
| 实际速度 / 电流 / 数字输入 | `0x606C:00` I32 / `0x6078:00` I16 / `0x60FD:00` U32 | 手册第 100 页确认了这些对象。输入位 0/1/2/3 分别表示负限位、正限位、原点和硬件锁定。 |
| 报警字 | `0x2601:00`、`0x2602:00` | 选择与固件相符的位；相互矛盾的手册表格不得渗入通用传输逻辑。 |
| 可选的附加诊断 | `0x2605:07`、`0x603F:00`、`0x2680:00`、`0x6077:00`、`0x6064:00` | 添加前请确认其表示和所选设备上的含义；位置条目/错误表示不可互换。 |

已知的手册换算包括速度 `DEC = rpm * 512 * encoder_resolution / 1875` 以及加速度
`DEC = (rps/s) * 65536 * encoder_resolution / 4000000`。请在模型/清单中确认编码器
分辨率、峰值电流、位置单位以及各命令的转矩/电流约定。不要仅因为 GUI 显示 rpm 或
Arms 就发送未缩放的工程值。

### 兼容性风险与条件行为

| 主题 | 保留的发现 / 需要注意之处 | 所属层 |
| --- | --- | --- |
| 运行模式 | 第 100 页列出 `1, 3, 4, -3, -4, 6, 7`；第 131/132 页描述 CANopen 插补模式 7。仅为实际型号选择所需模式；EtherCAT CSP/CSV/CST `8/9/10` 不能替代。 | 外部模型 / 测试契约 |
| 通信区的应用设置 | 手册提到 `0x103F` 立即目标更新和 `0x100B` 节点设置。自定义 JSON 对象限于 `0x2000..0xBFFF`，因此不能简单地作为应用对象添加。如被选定，请确认面向 CAN 的需求并添加受支持的集成/schema 处理。串口保存/重启说明和 CAN 拨码开关并不构成与 LSS 等效的设置路径。 | 集成 / 配置 |
| PDO 禁止时间单位 | 手册第 123/129 页写的是 ms；标准 `0x1800:03` 和 JSON `inhibit_100us` 使用 100 us。请核实原始值或抓包/GUI 换算；不要仅凭措辞修改协议栈单位。 | 配置 / 总线契约 |
| 发送/帧变体 | 手册中关于 29 位 ID 或所有传输类型的宽泛说法，不改变当前对 11 位/类型 0..240/254/255 的支持。不支持 RTR 触发的 TPDO 类型 252/253；守护 RTR 是另一项服务。 | 集成（仅在确实需要时） |
| 厂商波特率对象 `0x2F81:00` | 第 127 页编码为 `100=1000`、`50=500`、`25=250`、`12=125`、`5=50`、`1=10` kbit/s；默认编码 50 表示 500 kbit/s。RS232 的 38,400 bit/s 设置与此无关。当前写入只改变 OD 存储，而不改变适配器波特率。如实现，激活/保存时机需要确认。 | 可选的厂商适配器 + 通用重配置 |
| 保存/默认 `0x2FF0` | 第 106 页：U8 `:01=1` 存储配置/控制参数，`:01=10` 初始化配置，`:03=1` 存储电机参数。恢复默认后必须再保存（第 59 页）。存储的字节本身不会执行这些操作；如被选定，需要定义分区、完成和回读语义。这不是 LSS 持久化，也不是通用 CiA 402 行为。 | 外部设备适配器/模型，需要时配合通用钩子 |
| 中断 `0x6007` | 第 128/132 页描述 0=无动作、1=错误；当前协议栈无论如何都会报告寿命守护失败并离开 NMT Operational。忽略模型报警无法恢复 PDO 交换或撤销 EMCY。参见[策略限制](#deferred-communication-interruption-policy)。 | 通用集成策略 + 厂商解释 |
| 转矩/电流缩放 | 第 101 页对 `0x6071` 混用了额定转矩百分比和电流/DEC 的描述；`0x60F6:08` 是独立的电流命令。请确认所选的原始约定。 | 模型 / 清单 |
| 报警/状态位 | 第 67/107 页关于 `0x2601` 编码器错误位和 bit 14 的描述不一致；第 99 页状态字 bit 4 的措辞/极性也需注意。请确认实际固件的位映射以及模式相关的高位。 | 外部模型 |
| 错误表示 | 手册将 `0x2602` bit 12 / 内部错误 `0x8100` 与中断关联；这并未确立 EMCY 总线错误码或确切的制造商字节布局。当前故障 API 使用 CANopenNode 约定。 | 模型 / 设备适配器 |
| 插补观察 | 放宽的时序并未定义模式 7 的缓冲、锁存或哪些命令必须被观察到。在添加 SYNC 事件 API 之前请确认这些语义。 | 模型契约 / 通用事件边界 |
| 编辑性示例 | 第 118 页模式地址的介绍性措辞与对象表冲突；第 134 页将 `0x03EB` 标为 1000 ms，但 1000 是 `0x03E8`。请优先采用一致的对象定义和标准 SDO 帧格式，而不是孤立的字节示例。 | 配置 / 测试预期 |

<a id="maintaining-and-extending-the-integration"></a>

## 维护与扩展集成

### 工作线程与驱动流程

[src/device.cpp](src/device.cpp) 中的工作线程是帧分发和协议时间推进在运行时的唯一
所有者。它使用实际的单调流逝微秒数，而不是假定的名义周期：

```text
Receive one bounded nonblocking XNET read
    -> validate each frame and match ID/mask/RTR subscriptions
    -> dispatch registered stack callback
    -> service CO_process(..., dt=0) between frames for general protocols
    -> handle reset/bitrate-switch requests before continuing
CO_process(..., elapsed_us)
    -> handle reset, otherwise:
       CO_process_SYNC() -> CO_process_RPDO() -> CO_process_TPDO()
Drain pending adapter output
Wait worker_period_us, releasing the shared OD mutex
```

LSS 激活会在其要求的静默间隔内挂起接收/协议/发送处理。帧之间的通用协议处理并不能
使 RPDO 投递无损。主机模型独立执行，而不在此流程中。不要从 LabVIEW 引入第二条
未同步的协议栈处理路径。帧读写使用零超时的 XNET 调用；持久化是一条独立的慢速路径，
它释放 OD 互斥锁，同时保留生命周期/工作线程的所有权。同步日志仍可能在持有 OD 互斥锁
时执行磁盘 I/O。

[src/driver.cpp](src/driver.cpp) 实现了
[CANopenNode 驱动接口](../301/CO_driver.h)，其本身不负责 SDO/PDO 解码：

- `CO_CANrxBufferInit()` 记录 ID/掩码/RTR 订阅；接收分发会调用匹配的协议回调。内部帧
  位 `0x800` 表示 RTR，而不是第十二个 CAN-ID 位。需要与 XNET 独立的帧类型相互转换。
- 在访问负载之前校验流记录的长度和类型。即使是短帧也要保留 8 个稳定且零初始化的
  数据字节，因为某些协议栈回调会复制全部 8 个字节。忽略发送回显。
- `CO_CANsend()` 将副本排队，受协议栈发送缓冲区数量限制。`transmit()` 每次零超时
  写入提交一帧；队列满时保留该帧以便重试，并记录溢出。被队列接受不等于总线投递。
- `CO_CANmodule_process()` 转换硬件 CAN 状态/错误。
  `CO_CANclearPendingSyncPDOs()` 可以移除适配器中待处理的副本，但无法移除已被 XNET
  接受的帧。不要为了取消一个 PDO 而刷新无关流量。

### 协议栈分配与 OD 生命周期

[src/model.cpp](src/model.cpp) 拥有对齐的值、VAR/RECORD 描述符、排序后的 `OD_entry_t`
列表以及 `CO_config_t`。ARRAY 值使用独立的记录子条目。在将指针发布给协议栈之前完成
存储的最终化；初始化之后绝不要调整底层 vector 的大小。模型的生命周期必须长于协议栈。
关于协议栈布局参考，请参见 [OD 文档](../doc/objectDictionary.md) 和
[生成的示例 OD](../example/OD.c)，它们不是运行时配置输入。上游的
[空白驱动](../example/CO_driver_blank.c) 和 [main 模板](../example/main_blank.c)
并不是可用的 XNET/调度器代码。

协议栈设置顺序为 `CO_new()` -> `CO_CANinit()` -> `CO_LSSinit()` 及回调 ->
`CO_CANopenInit()` -> `CO_CANopenInitPDO()`。正常模式/会话的启动和清理由封装层协调。
检查每个返回值和初始化的 `errInfo`。`CANptr` 始终指向设备上下文，而绝不是 CAN 帧。
`CO_delete()` 会使协议栈拥有的扩展失效；复位代码会在保留模型存储的同时分离/重建它们。
保留封装层装饰过的通信回调，并在批量调用时解析新的 OD 流，而不是缓存过期的扩展指针
或通过直接内存写入绕过它们。

### PDO 映射内部机制

映射记录包含描述符 `[index:16][subindex:8][bit-length:8]`；描述符是配置，而不是负载。
例如，先映射 `0x2000:00` U32 再映射 `0x2001:00` U16，得到描述符字 `0x20000020`、
`0x20010010`。值 `0x12345678` 和 `0xABCD` 生成 6 个字节：`78 56 34 12 CD AB`。偏移
由映射顺序决定，而不是 OD 的数值顺序。本移植要求整字节宽度的值，不启用按位映射。

在 [CO_PDO.c](../301/CO_PDO.c) 中：

- `CO_TPDO_init()`/`CO_RPDO_init()` 使用 `PDO_initMapping()` 和 `PDOconfigMap()` 查找
  OD 条目、校验权限/长度并准备 OD I/O。
- `CO_TPDO_process()` 决定何时发送；内部的 `CO_TPDOsend()` 通过 OD 回调读取映射值，
  并提交打包后的帧。
- `CO_PDO_receive()` 缓冲输入；`CO_RPDO_process()` 随后通过 OD 写入应用它。该缓冲区
  不是事件队列。

协议栈的 `OD_write_PDO_mapping()`、`OD_write_18xx()` 和 `OD_write_14xx()` 在其他移植
中支持动态配置。本集成有意保持这些记录为只读。添加实时 PDO 配置是一个新的行为契约，
而不只是修改底层字节或启用一个标志；校验、扩展更新、复位默认值、主站预期和测试
都必须保持一致。

### 编译期配置

所有 C 协议栈源文件和 C++ 封装源文件必须使用相同的功能设置：宏会改变结构布局和 API
可用性。集成的 CMake 对象目标提供共享的包含路径。请将移植配置放在
[src/CO_driver_target.h](src/CO_driver_target.h) 中，而不是分散在各源文件的定义中。

| 层 | 当前选择 / 规则 |
| --- | --- |
| 分配 | `CO_MULTIPLE_OD` 提供每句柄的 `CO_config_t`/OD。保持 `CO_USE_GLOBALS` 未定义；将其定义为零仍会选中 `#ifdef` 分支。 |
| 能力掩码 | 用 `\|` 组合功能位；编译进来的功能本身不会创建服务实例或 OD 条目。 |
| 计数与 OD 指针 | `Model::configure()` 将服务计数/OD 条目与分配关联起来。保持计数、编译的源文件和描述符一致；不要修改内部的 `CO_RX_CNT_*`/`CO_TX_CNT_*` 索引计算。 |
| 运行时值 | JSON/SDO 通信值配置已启用的服务；它们不会启用被编译排除的服务。 |
| 支持的字节序 | 仅支持小端存储和目标；在未实现标量/总线转换之前，不要移除目标检查。 |

当前掩码启用了 NMT/心跳基础、节点守护从站、分段 SDO 服务器、EMCY 生产者/历史/
禁止时间/配置、带实时 OD 处理的 SYNC 消费者、带计时器/SYNC/OD I/O 的双向 PDO，以及
LSS 从站。SDO 客户端、心跳消费者、守护主站、SYNC 生产者、TIME、LED、网关、GFC、
SRDO、trace 和通用 OD 存储均被禁用。

重要的宏陷阱：

- `CO_CONFIG_NMT = 0` 仍保留基础 NMT/心跳；`CO_CONFIG_SDO_SRV = 0` 仍会保留加速 SDO，
  而不会移除服务器。
- `CO_CONFIG_FLAG_OD_DYNAMIC` 启用特定服务的实时 OD 处理。此处全局动态处理是关闭的；
  SYNC 显式选择启用，而 PDO 没有。不要为了添加单个服务回调而全局启用它。
- Callback-pre 标志启用的是唤醒回调，本身并不会创建工作线程。Timer-next 标志计算的是
  截止时间，而不是决定协议计时器是否存在。
- [CO_config.h](../301/CO_config.h) 记录了各标志，但许多示意掩码位于 `CO_DOXYGEN` 下；
  实际的回退掩码位于各协议头文件中。不要在应用构建中定义 `CO_DOXYGEN`。
- LSS 持久化使用其存储回调，不需要 `CO_CONFIG_STORAGE`，也不实现通用的 OD 存储/恢复。

上游编号文件夹标识的是 CiA 规范领域，而不是 OD 地址范围：`301` 是核心通信，`303`
包含 LED 建议，`304` 是 CANopen Safety，`305` 是 LSS，`309` 包含 ASCII 网关。添加
上游模块仍需要集成配置、OD 条目和处理逻辑。

### 变更检查清单

1. 判断该功能属于配置、外部设备模型、通用的面向模型钩子，还是通信/驱动层。复用
   CANopenNode 服务和 OD 扩展，而不是重新实现协议。
2. 对于 schema/类型/服务变更，按需更新校验、对齐存储/默认值、描述符、服务计数、
   功能掩码和 CMake 源文件。当字段可导入时，同时接入 EDS 转换和生成的输入说明。
3. 定义启动、主机停止/启动、两种协议复位、LSS 节点变更、反初始化/重新初始化以及
   清理失败时的行为。从模型默认值重新计算相对 COB-ID；不要意外保留旧节点的 ID。
4. 保持共享锁、扩展回调和工作线程所有权。绝不要在工作线程/存储操作仍可能使用某状态
   时释放它。通过操作/逐条目结果和诊断暴露错误，而不是返回看似成功的默认值。
5. 对于 ABI 变更，同时更新公共头文件、C ABI 断言以及 LabVIEW 描述符/调用节点。
   schema 版本 1 的可选字段仍可能需要较新的运行时；旧二进制会拒绝未知键。
6. 使用真实协议栈添加针对性的模拟 XNET 测试，并为配置变更添加转换器测试。覆盖与该
   变更相关的边界、权限、回调失败、复位保留、相对 ID/冲突以及失败/重试路径。
7. 更新本指南，并对依赖硬件的行为执行目标验收。模拟测试的成功无法证明总线时序、
   物理波特率变更、已安装 XNET 的兼容性或文件系统的持久性。

<a id="validation-and-limits"></a>

## 验证与限制

自动化测试组覆盖模型校验和数值限制、批量权限/部分错误/字符串缓冲区、并发调用方、
调用方管理的资源与清理、经由协议栈的真实 SDO/PDO/NMT/EMCY 通信、LSS 选择/查询/
节点 ID 变更、波特率静默时序、持久化/重新加载/失败路径以及同步手动请求。其他测试组
覆盖守护 RTR 过滤和翻转应答、寿命超时/恢复、心跳/守护切换、可配置 SYNC/EMCY、
冲突/溢出拒绝、SYNC 计时器/计数器以及通信设置的保留/复位。一个 C 源文件验证公共
头文件、40 字节状态布局、守护状态偏移未变以及追加的复位计数器偏移。复位状态测试组
覆盖完成计数、保留/复位的应用值、重复/广播命令、普通 NMT 状态变化、停止/启动、
拆除/重新初始化、复位/初始化失败以及 uint32 回绕。8 PDO 测试组覆盖全部 8 条发送/接收
路径、第 9 个槽位的拒绝、显式 ID 要求、高编号槽位冲突（包括实时 SYNC/EMCY 写入）、
空/禁用槽位、同步 PDO8、停止/启动、NMT 复位以及 LSS 节点变更。

开发时请使用 Windows/MSVC 或 Linux x86-64/GCC 模拟构建/测试，Linux 上可使用
ASan/UBSan。它们并不构成 NI Linux RT AMD64 硬件验收。

重要限制：

- 不提供 CiA 402 状态机、运动模型、库内 EDS 加载器或 VeriStand UI。提供独立的
  EDS 到 JSON 模板转换器。
- 工作线程使用继承的普通操作系统调度；无硬实时保证。
- 每次迭代的接收工作限定为一次 8 KiB 读取。输出以零超时每次写一帧，避免模糊的
  多帧部分写入。
- 适配器输出限定为每个协议栈发送缓冲区一个待处理副本。队列溢出会被报告而不是隐藏；
  XNET 队列满的帧会重试。
- 仍在适配器队列中的过时同步帧可以被移除。已交给 XNET 的帧无法通过 `nxFlush` 选择性
  取消。不保证严格的 SYNC 窗口；会话级刷新保留给停止/复位/波特率切换，而不是用于
  取消单个 PDO。
- 未实现由封装层控制的总线关闭重启/恢复，也未实现 FAILED 运行时的恢复。物理警告/
  被动/总线关闭标志会根据 XNET 状态重新计算，并可能在硬件恢复时清除，但这不会自动
  使 NMT 回到 Operational，也不会重启 FAILED 的运行时。

目标机上的手动验收应覆盖：同一端口的流输入/输出、启动报文和心跳、加速/分段 SDO、
在相应 NMT 状态下的固定 RPDO/TPDO、SYNC、复位、EMCY、包括波特率切换在内的完整 LSS
调试、选用时的守护 RTR/翻转/超时行为、可写的 SYNC/EMCY 配置，以及进程重启后的文件
重新加载。请验证队列行为、物理时序、已安装 XNET 的 ABI 兼容性以及文件系统的持久性
语义。

作为最小的首次硬件检查，请使用正确的终端电阻/接线、匹配的波特率和不同的节点 ID。
对于模拟器节点 10，启动工作线程，确认 `0x70A` 上的启动报文/心跳，然后通过
`0x60A`/`0x58A` 读取厂商 ID `0x1018:01`。在检查已知的固定 PDO 负载之前，先将节点
切换到 Operational。然后扩展到 RPDO 命令和外部模型反馈、命令节奏、故障/清除行为以及
两种复位路径。不要将 SDO 成功或 NMT Operational 视为已实现 CiA 402 行为的证明。
