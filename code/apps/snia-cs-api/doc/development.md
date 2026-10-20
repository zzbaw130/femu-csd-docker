# 开发与验证记录

分支 `snia_csd_dev`，基线 tag `femu-csd-with-original-postgre`。当前功能不包含 PostgreSQL SQL runtime。

## 已完成阶段

- 初始阶段：33 个 Guest API 入口、句柄与引用、SERIAL/PARALLEL/HYBRID、event/callback，以及原生 SO 加载/激活。
- `23aedf18f`：内部 AFDM→NVM（D1），LOAD/STORE 复用正常 FTL 时序与 GC。
- `557f05d9c`：通用 GET_INFO 和完整 64 位完成值；保持原生 femu_csd_args 结构。
- `0f7df7b18`：标准 STORE、临时区段与跨 batch spill；本节之后的参数实现已由下面的简化阶段替换。
- `9bad88739`：澄清原生参数和最小 spill 调用流程；本地 AGENTS.md 明确禁止过度工程化。

## 2026-10-19：删除参数描述层，简化重入

### 修改

- 删除额外参数解码头、描述结构、写入描述的 AFDM 及所有调用点，包括 workmem 探针。
- 库将 AFDM 按顺序直接映射为原生 MR，标量直接映射到 cparam1/2；当时曾实现128位映射，现已删除。超出两个标量位置、计算 AFDM 非零 offset 明确返回不支持。
- 原生 E1 支持零 MR，使已有纯标量函数和 workmem 用量查询无需占位 AFDM；拒绝零 MR 携带无效附加数据。
- 删除控制参数、版本、magic、事件与128位控制封装。重入算子保留 cparam2：首次为0，内部I/O完成后由库填为 spill_id。
- 设备侧提供 csIsReentry/csReentryValue，算子在入口判断并更新自己的状态；csNeedStore/Load/Free 用CompValue提出请求。普通结果直接完成，不再包装 DONE。
- 保留区段分配、跨 batch 标识、D0/D1 短路径、失败临时区回收和后继等待。私有查询对象由算子自己的清理入口负责。
- 示例直接读取原生参数；workmem 把字节数/参与者数放入其明确约定的输入结构，标量只传slot。重入示例的pending状态使用共享工作池，跨CU恢复。
- 中文 README 以 sort.sink 入口伪代码说明重入判断、记录 run 和继续执行，并解释sort.source使用同样方法。

### 验证

- mock API 与共享spill场景：原生MR/标量、两标量边界、普通128位输入、计算offset拒绝、无隐式AFDM分配、图重复执行、跨批spill、双分支汇合及失败回收。
- QTest csd-info-result64 同时验证有MR和零MR的完整64位完成值，csd-exec-concurrency回归。
- 222 项 mock API 检查及共享 spill 场景通过，ASan/UBSan/LeakSanitizer、C++头文件、33入口检查通过。
- workmem 本地池、8线程压力、两个RTLD_LOCAL SO共享配额与跨库释放检查通过。
- 重编新镜像 bbdb19c262fb，替换容器后健康；空白模拟盘重新执行既有挂载脚本。
- Guest 原生参数、纯标量、普通128位输入、4个CU执行重叠、跨批spill、重复图、双分支汇合和失败回收通过。
- 普通api-smoke改为通过算子约定的cparam2传递offset，输出 SNIA API batch PASS: 4 12 28 44。
- Guest workmem探针两轮均为4CU/2SO：2次分配成功、2次ENOMEM，池64MiB，峰值50331744字节，结束后占用为0。
- 已安装头文件和动态库的Guest应用构建运行通过；旧测试安装位置的解码头已清除。

## 当前边界

图由Guest库调度，设备无整图执行；查询程序目录仅进程内有效。spill标识不跨进程或设备重启持久化；运行中的native CSF不能抢占。设备统计、文件I/O、动态CSEE、主机映射和native沙箱不支持。原生参数限制在README和兼容性说明中明确列出。


## 2026-10-19：澄清参数来源与文档对象

- 对照 SNIA v1.1 第62/74页，说明普通128位参数来自标准枚举，与重入控制无关；当时添加的128位成员现已删除。
- 核对原生E1附加输入：MR之后的字节通过data_buffer/buffer_len交给CSF，本次执行结束释放，修改不自动回写。明确本库当前未映射该路径，以及Linux ioctl data_len与命令CDW4不是同一字段。
- 说明“最后一个AFDM”限定在当前CSF请求中，上例具体指io；先介绍batch中的请求与依赖，再区分batch和设备私有SortState的sort_handle。
- 用完整加入/提交/释放batch的示意代码替换跳跃叙述，解释复制请求描述和保留句柄的实际含义。仅文档及头文件注释变化，git diff --check通过。


## 2026-10-19：统一 SNIA 命名，保留两个独立 cparam

- 应用与设备组件目录统一为 snia-cs-api，库名为 libsnia-cs-api.so/.a，示例为 snia-cs-scale.so，测试入口为 tests/snia-cs-api-test.sh。
- 同步构建依赖、导出符号版本名、头文件保护宏、示例加载描述、测试路径与所有项目文档中的相关拼写；本地分支改为 snia_csd_dev，保留提交历史。
- 删除128位值成员、helper取值和拼接/拆分路径。两个原生long long字段独立传递，128位类型保留标准枚举但明确返回不支持。
- 设备示例与真实测试用7、3两个独立64位值检查cparam1/2，更新mock中的不支持类型检查；删除测试脚本中已废弃的手工资源配置。

### 本阶段验证

- 更名后的测试脚本build/smoke通过；231项接口检查、33个标准入口和C++头文件检查通过。
- 7、3独立传入cparam1/2并正确匹配，交换顺序按算子约定失败；128位请求明确返回CS_UNSUPPORTED_TYPE。
- ASan/UBSan/LeakSanitizer检查通过；设备程序统一workmem动态依赖检查通过。
- Guest真实NVMe测试覆盖4CU并行、内部STORE/LOAD、跨批spill与失败回收；workmem两轮跨SO配额验证通过。
- 使用新安装头文件和-lsnia-cs-api动态链接应用，ldd确认加载libsnia-cs-api.so，示例输出4 12 28 44。
- 项目自有源码、构建脚本和文档无旧拼写引用，旧生成库名已清理；shell语法和git diff --check通过。

## 2026-10-20：补齐 Guest CMake

- docker-compose.yml 的 command 覆盖 Dockerfile CMD，创建 Guest 的包列表遗漏 cmake；已与 Dockerfile 中的依赖同步。
- 当前 Guest 直接通过 apt 安装 cmake，容器与 Guest 均验证为 3.28.3；不重新创建镜像或清空模拟盘。后续项目使用 CMake 3.16 兼容语法。

## 2026-10-20：CMake、单头文件和运行时设备插件

### 修改

- 原 Makefile 替换为 CMake，公共库、FEMU 插件及测试分别构建；仅发布动态库。FEMU 构建和测试规则分别位于各自目录的 CMakeLists.txt。
- cs_types.h 合入 cs.h，删除 cs.sh；include/ 只保留 cs.h，公开类型、状态和33个函数签名保持不变。FEMU 原生参数和重入辅助头移至 plugins/femu/include/。
- src/facade/ 按 SNIA 分类分为九个文件；AFDM 移动归 cs_afdm_management.c，通知归 cs_access.c，设备查询归 cs_management.c，CSEE 下载归 cs_csf_scheduling.c。
- src/core/ 保留公共句柄及引用、异步请求、event和batch依赖；公开句柄绑定到插件不透明对象，不再包含 NVMe fd、namespace 或原生程序字段。
- src/interface/cs_backend.h 定义 C 操作表及 create/destroy 工厂契约。插件按公共库相邻目录自动发现，也支持 csRegisterPlugin/csDeregisterPlugin；在设备、资源或发现操作引用存在时拒绝卸载。
- plugins/femu/ 独立编译 libcs-femu.so，包含 Identify、GET_INFO、NVMe 传输、内存、程序、参数映射及 spill。NEED_STORE/LOAD/FREE 由该插件解释，通用 batch 不占用这些完成值。内部 I/O 直接调用插件存储实现，不回调公共 API 或公共句柄表。
- 同步更新设备算子的头文件路径、workmem 应用的 CMake SDK 构建和动态链接、既有测试脚本及中文文档。FEMU 设备源码及原生参数 ABI 未修改。

### 验证

- Debug 和 RelWithDebInfo 构建均通过；5组 CTest 通过，包含231项既有 API 检查、Identify、C++单头文件及静态边界检查。测试程序明确保留断言，避免发布配置跳过带调用的检查。
- 第二个非 NVMe mock 插件实际通过 dlopen 加载：验证两插件发现汇总、不透明函数/内存标识绑定、三标量与非零偏移、异步event、HYBRID和跨设备拒绝；FEMU保留范围的完成值在其他插件正常返回，不触发spill。
- 插件重复注册、类型不匹配、缺失库、引用期间拒绝卸载、释放后注销/重新加载验证通过。公共库实际只导出33个API，FEMU插件只导出两个工厂入口。
- ASan/UBSan/LeakSanitizer 检查通过；LSan 在沙箱外执行，避免 ptrace 限制。
- Guest真实NVMe回归通过：4CU执行重叠、独立64位cparam、64位完成值、内部STORE/LOAD、跨batch spill、失败回收及重复执行。
- Guest workmem 两轮通过：4CU/2SO共享64MiB，2次成功/2次ENOMEM，峰值50331744字节，结束占用0；5个设备SO的共享workmem依赖检查通过。
- Guest已安装到/usr/local并执行ldconfig；独立应用仅包含cs.h，以 cc application.c -lsnia-cs-api 编译并实际运行成功，插件从安装目录自动加载。
- 当前重建后的模拟盘无分区或文件系统签名，沿用既有mount-nvme.sh准备80%数据区和20%spill区用于真实测试；未重启容器或Guest。

## 2026-10-20：标准后端命名与扁平化实现

### 修改

- 明确当前通过 C 函数指针表实现运行时多态，不是 C++ 虚函数类。
- 后端标准操作统一使用原 API 去掉 cs 前缀的名称，例如 queryDeviceProperties、getCSFId、queueComputeRequest；FEMU 实现同步改为 femu_ 对应名称。
- 将原先混用的 execute 分成 queueComputeRequest、queueCopyMemRequest、queueStorageRequest 三个强类型方法；通知也分为 registerNotify/deregisterNotify。
- 标准操作与 getCSFResourceId、validateRequest、executeBatchEntry、finishBatchExecution 四个内部钩子分组，逐项中文说明用途、调用时机及其不属于标准 API。
- 删除 src/facade、src/core、src/interface 目录层级。九个标准分类文件直接位于 src/；batch 配置/调度合入 cs_batch_scheduling.c，动态插件加载/注册合入 cs_library_management.c，设备关闭直接在 cs_access.c 实现，删除同义薄包装。
- 仅保留 cs_handle_registry.c 与 cs_request_execution.c 两份跨 API 共享实现；共享函数按句柄 acquire/retain/release、请求 prepare/execute/submit 等具体用途命名，文件顶部说明职责。删除未被读取的第二份请求副本。
- src/cs_backend.h 为插件契约，src/cs_internal.h 为公共实现私有对象；同步 CMake、边界检查、mock 插件与中文 README/架构文档。公开 cs.h 与33个API签名不变。
- 操作表布局变化，内部插件 ABI 更新为2，旧插件需重编；已同步重编并安装 Guest 动态库与插件，不改变设备算子 ABI。

### 验证

- 五组 CTest 通过：231项API检查、单头文件C++、Identify、两种插件路由，以及后端命名/目录边界。
- ASan/UBSan/LeakSanitizer 全部通过；确认公共库导出的仍是原33个SNIA函数。
- 用原先ABI 1 mock插件验证新加载器明确返回CS_LOAD_ERROR，避免错误解释旧操作表。
- Guest真实回归通过：四CU重叠执行、完整64位参数/完成值、内部STORE/LOAD、跨batch spill、重复执行和失败回收。
- Guest重新安装到/usr/local并执行ldconfig，独立应用仍以 cc application.c -lsnia-cs-api 编译运行成功，无需额外头文件或显式链接设备插件。

## 2026-10-20：用 C++ 简化内部资源管理

### 修改

- 公共实现使用 C++17，九个 SNIA 分类文件以及句柄、请求共享实现改为 .cpp，私有头改为 cs_internal.hpp；保持现有扁平目录和 API 分类。
- Handle 提供虚析构，Resource 提供所属设备和私有标识访问，Device、Memory、Function 等具体资源类使用 final。句柄查找返回带类型的智能指针，设备、插件、AFDM、请求之间通过引用管理生命周期。
- 请求缓冲用 unique_ptr 管理，batch 配置使用 array 草稿，event 完成队列使用 list，锁由 lock_guard/unique_lock 管理。删除手工引用增减、解锁和 goto 回滚；显式释放失败时 CloseHandle 自动恢复句柄。
- 异步请求在提交前分配完成通知节点，完成时直接移入 event 队列。C API 和工作线程统一捕获内部异常，转换为 CS_STATUS；分配失败时自动释放资源。
- 公开 cs.h、33个C函数签名和插件 cs_backend.h 均保持原样，插件 ABI 仍为2。现有 FEMU 和第二个测试插件继续使用 C 实现。
- 公共实现及私有头共由1379行减为1045行；补充中文 README 与架构说明，解释具体资源的所有权和调用边界。

### 验证

- 宿主 GCC9、容器 GCC13 均通过六组 CTest，覆盖231项既有 API 检查、两插件路由、Identify、单头文件 C++、33个入口及新增分配失败检查。
- 分配失败测试覆盖创建 AFDM 后发布句柄失败、创建 event 失败，以及128次异步提交中的分配失败和重试；检查完整 FDM 容量可重新申请、无丢失通知、句柄和资源最终可释放。
- 六组测试均通过 ASan/UBSan/LeakSanitizer；确认动态库仍只导出原33个 C API。
- Guest 真实设备回归通过：四 CU 执行重叠、独立64位参数/完成值、内部 STORE/LOAD、跨 batch spill、失败回收及重复执行。
- Guest 重新安装至 /usr/local；独立 C 应用只包含 cs.h，以 cc application.c -lsnia-cs-api 编译运行，输出4 12 28 44。
- 设备五个 SO 的共享 workmem 依赖检查通过；Guest 两轮4CU/2SO共享64MiB配额测试通过，均为2次成功、2次ENOMEM，结束占用0。

## 2026-10-20：目录、CMake 与完整文件排序示例

### 修改

- src/core 保存九个标准分类 CPP；共享句柄/请求实现与更名后的 cs_common.hpp 移入 src/common；插件契约更名为 src/interface/cs_backend_interface.h；设备适配器移入 src/plugins。
- FEMU 文件统一使用 femu_功能分类命名，将设备访问、AFDM复制、计算、存储和属性查询放入对应分类；共享传输、目录、资源查询、验证和重入分派的职责写入插件 README。公开 cs.h 与插件 ABI 2 不变。
- 测试改按 SNIA 功能命名，删除只检查源码名称/目录的 Python 脚本；保留实际 API、插件路由、资源失败、Identify 和真实 batch 检查，并提供测试目录说明。
- csd-programs 全部改用根 CMakeLists.txt 管理；add_csd_program 统一 workmem 动态依赖、输出路径和 ORIGIN。删除 Makefile、mk/、独立 snia-cs-api 算子目录和 examples/build.sh。
- workmem 接口/实现平铺，合并使用规范到 README，删除 doc/；Guest 客户端和探针迁入其 tests/，移除 apps/csd-workmem-test 和本分支空的 apps/pg_csd 目录。真实客户端通过 SSH 传至 Guest 临时目录构建。
- 原 SNIA scale/并行/spill 回归函数统一放在 csd-programs/examples/snia_compute_functions.c，产物改为 cs-example-functions.so。
- 新增 Guest cs_batch_sort_limit.c、共享数据布局、设备 sort.cpp 和 limit.c。生成256MiB随机 int32 文件，FIEMAP 分区物理地址加分区起始位置，按逻辑4MiB收集一个或多个 extent 提交内部 LOAD。
- 第一阶段 batch 为 LOAD → sort_sink，生成64个降序 run，每个 run 通过16次 NEED_STORE 保存；第二阶段 batch 为 sort_source → limit_five，通过 NEED_LOAD 多路归并后取全局前5；最后 NEED_FREE 释放全部1024个块。私有算法内存通过 workmem 分配，约20MiB；错误路径释放状态并关闭设备回收 spill。
- 新增完整示例说明，逐项列出 input/io/sorted/result AFDM 和重入参数；同步根 README、SDK README 和测试脚本。

### 验证

- 按要求清空原 csd-programs/build，包括 legacy；容器 GCC13 用 CMake 重新编译8个 SO，全部 native 算子均保留 libcs-workmem.so 依赖和 ORIGIN 搜索路径。
- 宿主与容器的4组设备测试通过；完整归并测试比较全部3个 run 的输出，覆盖跨块/跨输出chunk、重复值、负数、EOF、LIMIT、FREE 和异常销毁。排序/workmem 通过 ASan/UBSan/LeakSanitizer。
- 宿主5组 SNIA 功能测试通过，保留231项既有 API 检查；公共库仍只导出33个C函数。
- Guest 真实 batch 回归通过，覆盖四CU重叠执行、独立64位参数、完成值、内部STORE/LOAD、跨batch spill及失败回收。
- 迁移后的 workmem Guest 客户端两轮通过：4CU/2SO共享64MiB，均2次成功/2次ENOMEM，结束占用0。
- Guest256MiB完整示例通过；分别遇到4个和3个文件extent，均读完64个输入块，结果为2147483640、2147483488、2147483485、2147483458、2147483243，与生成时独立维护的参考值一致。
- SDK重装至Guest /usr/local后，独立C应用仅链接-lsnia-cs-api，完整文件排序再次通过。保留/data/cs-batch-input.bin供查看；未重启Guest或重新分区。

## 2026-10-20：内存优先的双入口排序

### 修改

- sort.cpp 只保留 sort_sink、sort_source 两个设备入口，移除独立 create/destroy/abort。首次 sink 创建 context 并返回64位状态标识；source 直接读取该状态中的全部 run，最后一个输出 chunk 生成后 FREE token、销毁 context，并输出 finished=1。
- 每个4MiB run 优先申请 workmem 并驻留。只有申请失败才淘汰一个驻留 run，完整保存为16个256KiB块，再复用腾出的内存；source 的归并读缓冲也按申请失败触发淘汰，没有固定 spill 阈值。
- source 每次输出最多4MiB，全局降序流跨多个 batch 连续输出。limit 保留第一次选出的前5，并传递 finished；示例读到 EOF 展示自动销毁。提前停止或错误也使用同一 source（仅传 I/O AFDM）释放私有内存，随后关闭设备回收残留 token。
- 删除 snia_compute_functions.c 和 cs-example-functions.so，不再保留 scale/delay/标量/合成spill设备函数。删除依赖这些函数的独立真实测试及场景头；基础复制、storage、event、依赖图检查保留，计算和跨batch spill 改为直接测试真实 sort/limit。
- 第一个 sink 的 CompValue 取得状态标识后更新后续请求，程序槽位减少到420–422；仅 LIMIT 的小型累积结果需要清零，输入/排序输出由生产者覆盖。同步 AFDM 布局与中文文档，合并 smoke/example 测试入口。

### 验证

- 宿主5组 SNIA 功能测试通过；使用12MiB工作池的mock请求覆盖驻留run、申请失败spill、跨batch归并、event与EOF回收。
- 宿主与容器4组设备测试通过。排序测试确认内存充足时STORE次数为0、内存不足时才spill，并额外占用工作池触发source的淘汰；逐项比较完整排序流，覆盖负数、重复值、满chunk EOF和提前结束，最终context/占用归零。
- 排序和workmem的ASan/UBSan/LeakSanitizer全部通过。
- Guest64MiB workmem下的256MiB文件实测通过，完整归并后前5与参考一致；迁移后的workmem真实客户端两轮配额测试也通过。
- 容器已重编两个算子，排序SO仅有sort_sink/source两个设备入口；旧演示SO已删除。

## 2026-10-20：整理 batch 示例的请求构造

- 删除 copy_memory 薄封装，读取结果处直接构造 CsCopyMemRequest 并调用 csQueueCopyMemRequest。
- compute 改为 build_compute_request，只填写调用者提供的 CsComputeRequest；外层 CsBatchRequest 的 ReqType 在 main 显式设置，中文说明区分计算描述和 batch 节点。
- LOAD 与计算节点各只分配一份缓冲；配置 API 保存副本后重复使用，删除逐块 LOAD 分配和 sink/source/limit 各自的堆对象。load_extents 改为 build_load_request，同样只填写已有存储请求。
- 删除 single_batch，提前结束时直接调用 source 的单次计算请求；设备程序使用有名称的索引，主流程按 LOAD→sink、source→limit 两阶段排列。参考前5的更新直接放进文件生成循环，sysfs/设备几何读取函数使用明确动词命名。
- 宿主和 Guest 目标均编译通过；Guest 256MiB完整文件测试通过，覆盖复用请求、extent LOAD、按需spill、全量归并和EOF回收，前5仍与独立参考一致。
