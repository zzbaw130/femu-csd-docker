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
