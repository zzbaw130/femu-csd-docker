# 开发与验证记录

分支 `snia_csd_dev`，基线 tag `femu-csd-with-original-postgre`。当前功能不包含 PostgreSQL SQL runtime。

## 2026-10-21：统一项目名称和内部目录，新增 Guest 初始化入口

- 项目目录、CMake 项目/目标和动态库统一命名为 snia；应用包含 cs.h 并链接 -lsnia。默认插件目录为动态库旁的 snia/，安装位置为 /usr/local/lib/libsnia.so 和 /usr/local/lib/snia/libcs-femu.so，设备侧头文件安装到 include/snia/femu/。
- 九个标准分类实现保留在 src/core；句柄表、请求执行和 cs_internal.hpp 移到 src/core/internal。FEMU 根目录保留七个现有标准分类实现，femu_internal.h 及工厂入口、目录、资源查询、传输、请求验证/执行移入插件 internal/，不新增空的分类文件。
- 初始化脚本改名 init-csd-env.sh：保留原80%数据区、20%spill区的标签检查与挂载，增加 Guest 编译安装 SNIA、执行 ldconfig。标签匹配时复用已有分区；标签不匹配仍会清除模拟盘数据。scripts/build.sh 的 -p 流程同步使用新入口；首次 Guest 制作包列表加入 parted。
- 测试入口改名 tests/snia-test.sh；同步设备程序、workmem 调用方、插件发现路径、CMake、忽略规则和中文文档中的项目/库/脚本名称。历史记录中的路径与库名也统一采用当前名称，避免复制旧命令。
- 宿主设备程序5项和SNIA 5项CTest全部通过；容器设备程序和Guest SDK重新编译成功。workmem客户端以新路径和-lsnia重编，真实设备两轮测试均通过，结束后已用内存为0。初始化脚本在现有标签匹配的Guest上执行成功，完成安装和ldconfig；清除旧名称的已安装库、插件、设备头及项目内历史构建产物。
- 安装验证直接执行 cc -O2 -Wall -Wextra -Werror /home/femu/apps/snia/examples/cs_batch_sort_avg.c -lsnia，无额外-I/-L/rpath；ldd确认加载/usr/local/lib/libsnia.so，插件自动发现后完成256MiB排序。结果count=67108864、sum=15688155813636、average=233771.73861318827；60个spill run，内部写入/回读各240MiB；输入SHA256在初始化及执行前后均为9dde99b8dcf24e5c1fe46405f898a7cd955e59d3abb3a8845ccaf93602ae4d2b。

## 2026-10-21：精简示例，只保留 merge sort

- 删除设备 merge-path-sort.cpp、CPU 对照 read_merge_path_sort_avg.c、共享分区头和对应测试，移除 CMake 构建目标及 Guest 的 --merge-path 选项。原生 vadd/kernels 示例保留。
- Guest 只保留四条 LOAD→sort_sink 的 HYBRID 分支，以及单条 sort_source→AVG 的 SERIAL 循环；输出和累计结果各保留一个 AFDM，删除分区任务、双路结果合并和分支退出逻辑。
- bench 只交替运行 merge sort 的 CPU/CSD 各三轮，保留完整结果、spill 工作量、64MiB 内存预算核对和细分计时。当前 README 删除两路版本的用法和重复历史测量；并行归并的历史数据仍保留在下节。
- 宿主设备程序 5 项、SNIA SDK 5 项 CTest 全部通过，容器与 Guest 重新编译成功。清除容器构建目录、宿主测试目录和 Guest 中已移除版本的 SO/可执行文件，以及容器构建目录中对应的目标中间文件。
- Guest 复用已有 256MiB 文件验证 CPU/CSD 两条路径：count=67108864、sum=15688155813636、average=233771.73861318827；均为64个run、60个spill run，写入/回读各251658240字节。算法内存上界CPU66104320、CSD66135168字节，均低于64MiB。单次验证耗时CPU7.319867秒、CSD8.922484秒，仅用于确认清理后的完整执行，不作为新的多轮性能结论。

## 2026-10-21：保留 merge-sort，新增两路 K-Way Merge Path

> 历史实验：当前已移除两路 Merge Path 的实现、构建目标及命令行入口，以下数据仅保留为实验记录。

- 原sort.cpp原样更名merge-sort.cpp，SO更名merge-sort.so；CPU基线更名read_merge_sort_avg.c / read-merge-sort-avg。同步CMake、下载描述、测试、脚本和当前文档；清除canonical build中的旧cs-example-sort.so和read-sort-avg，不保留重复产物。
- 新增独立设备文件merge-path-sort.cpp / merge-path-sort.so、独立CPU文件tests/read_merge_path_sort_avg.c / read-merge-path-sort-avg。两个版本各保留sink/source两个CSF入口，第一阶段都是四路sink，新增版本第二阶段为两路source→AVG。Guest通过--merge-path选择，默认仍为merge-sort；不修改FEMU和SNIA库。
- examples/merge_path_sort.h共享精确co-rank算法及示例分区参数。按剩余秩计算步长、比较各run候选并推进安全前缀，重复值按run编号确定顺序；精确分为N/2和N−N/2条，不是近似的值域分组。设备通过NEED_LOAD读取spill中的探测点，CPU通过文件pread，均计时。
- source 0首次调用接管四组run，按申请失败触发spill，准备两份读缓存并计算边界，再输出第一块。之后Guest通过HYBRID构造两条source→AVG分支，每个分支有独立task/io/output/result AFDM。到EOF后从batch移除该分支，最终由主机相加两个sum/count得到AVG。
- Source持有独立游标、堆、缓存和重入状态。并行执行期间run和token只读；先完成者先把结果和计时复制到AFDM，再原子发布完成标志，此后不访问共享状态。最后完成者FREE全部token并销毁四个context，避免并发回收造成悬空引用。
- 两路版本从64MiB中扣除26,259,456字节AFDM预算，剩余40,849,408字节平均分给四个context；CPU采用相同预算。两份256KiB/run缓存最多32MiB，内存足够，但输入/输出缓冲和双缓存挤出了驻留run。算法保留申请失败才spill的规则。
- 新增sortKWayMergePathAverage：400组随机小run，检查0/中点/全部秩；实际CSF检查随机值、全重复、极值倾斜、两种完成顺序的逐项全局排序、完整AVG、spill、EOF与提前收尾。宿主6组设备测试＋5组SDK测试通过，容器新测试通过，ASan/UBSan/LeakSanitizer通过。未在性能测量期间并行运行这些压力测试。

### 同批性能对比

同一256MiB文件、3个extent、64MiB算法预算，各方法交替3轮。单位秒；每行分别为merge-sort CPU / Merge Path CPU / merge-sort CSD / Merge Path CSD总延迟：

| 轮次 | CPU单路归并 | CPU两路归并 | CSD单路归并 | CSD两路归并 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 7.610870 | 5.822173 | 8.345484 | 7.501335 |
| 2 | 7.459820 | 5.653753 | 8.492564 | 7.136502 |
| 3 | 7.328432 | 5.716957 | 8.513495 | 7.251252 |
| 平均 | 7.466374 | 5.730961 | 8.450514 | 7.296363 |

- 阶段1平均CPU单/双路2.646397/2.587540秒，CSD单/双路2.519082/2.488781秒。阶段2（含准备、边界、归并、AVG、回收）分别CPU4.819976/3.143420秒，CSD5.931433/4.807581秒。
- CPU整体加速1.303倍、延迟下降23.2%；CSD整体加速1.158倍、延迟下降13.7%。阶段2加速分别1.533/1.234倍。基线为本轮重新测量的保留版本，不能混用前一阶段8.013秒的历史均值。
- 全部12次count=67108864、sum=15688155813636、average=233771.73861318827。每种算法的CPU/CSD最终run数量、spill数量与读写字节完全一致。原文件SHA256保持9dde99b8dcf24e5c1fe46405f898a7cd955e59d3abb3a8845ccaf93602ae4d2b。
- merge-sort为64个run、60个spill run，写入/回读各251,658,240字节（240MiB）。两路版本64个run全部spill，写入268,435,456字节（256MiB）；回读597,164,032字节（569.5MiB），其中312,213,504字节（297.75MiB）用于边界探测，其余271.75MiB为两路归并读取，含各分区边界块的重复读取。
- 两路内存上界CPU66,133,632字节、CSD66,165,632字节，低于67,108,864字节。准备平均CPU0.101704秒、CSD0.070134秒，边界计算CPU0.262608秒、CSD0.526131秒，均计入阶段2。
- CSD的E1从3072次增至4518次；D0从1024次增至2342次，D1/TRIM从960增至1024次。并行计算收益受到额外探测、重复读取和请求重入的抵消，第二阶段不能直接减半。累计调用时间存在重叠，不与阶段墙钟相加。
- 原始每轮日志、timings.tsv、profiles.tsv：/tmp/snia-merge-compare.0kvahH。Docker镜像和FEMU进程未更换，本阶段直接加载新SO。

## 2026-10-21：四路 sink、单路全局归并及主机对照

- Guest第一阶段改为8节点HYBRID batch，四条LOAD→sort_sink分支，每轮16MiB、共16轮。四个sink使用同一程序ID420；每条分支独立的input/task/io AFDM，task中保存该分支的状态句柄。第二阶段仍为SERIAL sort_source→avg，完整消费所有输出。
- sort.cpp保留sink/source两个入口。新增三MR示例布局：输入或输出、任务/四句柄交接AFDM、最后一个spill I/O AFDM。source首次接管四个context，只复制run描述；每个run保存自己的分配器归属，用原context分配读缓冲、释放内存，EOF时统一FREE token并结束四个context。交接AFDM中已转移的句柄清零，错误收尾避免重复释放。
- 从64MiB扣除全部缓冲22,048,768字节，再将45,060,096字节工作区平均分给四个context；CPU保留同样预算和配额。按分配失败触发spill，source只淘汰申请读缓冲的同一个context内的驻留run。内存上界用各context峰值之和加缓冲保守统计。
- CPU基线使用四个pthread读交错分配的输入块、原地排序并spill到同一O_DIRECT临时文件，用原子序号避免写入区段冲突。join后主线程接管四组run，用同一64槽大根堆全局归并和AVG。
- 扩展现有native测试，新增sortParallelSinkMergeAverage：四线程调用同一函数，全部输出逐项对照std::sort；施加31MiB其他context压力，验证source按run归属淘汰；检查spill读写/free、EOF和接管后提前收尾均无内存残留。宿主5组设备程序＋5组SDK测试通过，容器5组设备测试通过，两个排序场景ASan/UBSan/LeakSanitizer通过。
- 正常关闭Guest并重启原容器，启用b4bbe5a51的同程序ID并发。模拟盘仅有测试文件，先备份到系统盘，再通过已有挂载脚本恢复分区和原文件；恢复前后SHA256均为9dde99b8dcf24e5c1fe46405f898a7cd955e59d3abb3a8845ccaf93602ae4d2b。系统盘保留、Docker镜像未重建，当前FEMU进程已运行新二进制。

### 实测

256MiB同一输入、64MiB算法总预算、4路sink/1路source。交替测量3轮，阶段1/阶段2/总延迟，单位秒：

| 轮次 | CPU阶段1 | CPU阶段2 | CPU总计 | CSD阶段1 | CSD阶段2 | CSD总计 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 2.544367 | 5.008515 | 7.552882 | 2.482426 | 5.366794 | 7.849219 |
| 2 | 2.534141 | 4.915638 | 7.449779 | 2.378141 | 5.404222 | 7.782363 |
| 3 | 2.568671 | 4.861266 | 7.429938 | 2.721210 | 5.684924 | 8.406133 |
| 平均 | 2.549060 | 4.928473 | 7.477533 | 2.527259 | 5.485313 | 8.012572 |

- 六次均count=67108864、sum=15688155813636、average=233771.73861318827；64个run、60个spill run，写入/回读各251,658,240字节。峰值上界CPU66,104,320字节、CSD66,135,168字节，均小于67,108,864字节。并行原始日志和TSV在/tmp/snia-sort-avg.Yv1VKK。
- 从b4bbe5a51取单线程CPU和Guest应用，在/tmp构建临时程序；当前SO仍支持双MR单sink ABI。相同环境的三轮总延迟CPU13.595184/13.589016/13.770298秒，CSD14.435705/14.145926/14.403529秒，平均13.651499/14.328387秒。两边单路spill均54个run、216MiB读写。原始记录在/tmp/sort-serial-control.gWE4Mj。
- 四路sink整体加速CPU1.826倍、CSD1.788倍；阶段1由8.584020/8.714043降至2.549060/2.527259秒，约3.368/3.448倍。比较包含并发缓冲增多、配额分摊和spill由54增至60的影响。
- CSD四路总计比CPU慢约7.2%，主要在阶段2：source执行平均4.735秒对CPU4.688秒，阶段总计5.485秒对4.928秒；剩余时间包含I/O、重入、调度、AVG及回收。E1/D0/D1累计等待在HYBRID中重叠，不能相加当作查询墙钟。

## 2026-10-20：允许同一程序 ID 在多个 CU 并发执行

- 删除设备程序的 busy 字段和按程序 ID 选择可运行任务的扫描。CU 线程直接领取队首请求；并发仍由 nr_cu 个线程限制，主机通过 batch 依赖安排执行顺序。
- 保留每个请求独立的参数、MR 数组、程序和 AFDM 引用计数。允许同一函数并发调用不会复制 SO 全局变量、AFDM 数据或 sort_handle；共享可变状态的同步由算子或主机负责。共享 workmem 总预算保持不变。
- 设备程序 README 补充多 CU 调用约定，明确当前 sort_handle 仍需串行调用；更新 FEMU 执行路径与限制说明。
- 修改现有 csd-exec-concurrency 回归：验证同一 ID 的两个任务重叠，以及 CU 全满时第三个任务等待，同时保留队列删除、重置及控制器持续响应检查。
- 容器内增量编译 FEMU 和 qos-test。相同测试在旧二进制上因两次1秒调用合计2.001秒而按预期失败；新二进制合计1.001秒，第三个100毫秒任务等待后于1.100秒完成。csd-exec-concurrency、csd-runtime-floor、csd-info-result64 三项通过。
- 使用独立 qtest 实例验证，现有 Guest 未重启，因此当前正在运行的 FEMU 进程仍是旧版本。容器可写层中的二进制已更新，Docker 镜像未重建。可选 uBPF 未启用，本阶段不声称验证其并发执行。

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

- 应用与设备组件目录统一为 snia，库名为 libsnia.so/.a，示例为 snia-cs-scale.so，测试入口为 tests/snia-test.sh。
- 同步构建依赖、导出符号版本名、头文件保护宏、示例加载描述、测试路径与所有项目文档中的相关拼写；本地分支改为 snia_csd_dev，保留提交历史。
- 删除128位值成员、helper取值和拼接/拆分路径。两个原生long long字段独立传递，128位类型保留标准枚举但明确返回不支持。
- 设备示例与真实测试用7、3两个独立64位值检查cparam1/2，更新mock中的不支持类型检查；删除测试脚本中已废弃的手工资源配置。

### 本阶段验证

- 更名后的测试脚本build/smoke通过；231项接口检查、33个标准入口和C++头文件检查通过。
- 7、3独立传入cparam1/2并正确匹配，交换顺序按算子约定失败；128位请求明确返回CS_UNSUPPORTED_TYPE。
- ASan/UBSan/LeakSanitizer检查通过；设备程序统一workmem动态依赖检查通过。
- Guest真实NVMe测试覆盖4CU并行、内部STORE/LOAD、跨批spill与失败回收；workmem两轮跨SO配额验证通过。
- 使用新安装头文件和-lsnia动态链接应用，ldd确认加载libsnia.so，示例输出4 12 28 44。
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
- Guest已安装到/usr/local并执行ldconfig；独立应用仅包含cs.h，以 cc application.c -lsnia 编译并实际运行成功，插件从安装目录自动加载。
- 当前重建后的模拟盘无分区或文件系统签名，沿用既有init-csd-env.sh准备80%数据区和20%spill区用于真实测试；未重启容器或Guest。

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
- Guest重新安装到/usr/local并执行ldconfig，独立应用仍以 cc application.c -lsnia 编译运行成功，无需额外头文件或显式链接设备插件。

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
- Guest 重新安装至 /usr/local；独立 C 应用只包含 cs.h，以 cc application.c -lsnia 编译运行，输出4 12 28 44。
- 设备五个 SO 的共享 workmem 依赖检查通过；Guest 两轮4CU/2SO共享64MiB配额测试通过，均为2次成功、2次ENOMEM，结束占用0。

## 2026-10-20：目录、CMake 与完整文件排序示例

### 修改

- src/core 保存九个标准分类 CPP；共享句柄/请求实现与更名后的 cs_internal.hpp 移入 src/core/internal；插件契约更名为 src/interface/cs_backend_interface.h；设备适配器移入 src/plugins。
- FEMU 文件统一使用 femu_功能分类命名，将设备访问、AFDM复制、计算、存储和属性查询放入对应分类；共享传输、目录、资源查询、验证和重入分派的职责写入插件 README。公开 cs.h 与插件 ABI 2 不变。
- 测试改按 SNIA 功能命名，删除只检查源码名称/目录的 Python 脚本；保留实际 API、插件路由、资源失败、Identify 和真实 batch 检查，并提供测试目录说明。
- csd-programs 全部改用根 CMakeLists.txt 管理；add_csd_program 统一 workmem 动态依赖、输出路径和 ORIGIN。删除 Makefile、mk/、独立 snia 算子目录和 examples/build.sh。
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
- SDK重装至Guest /usr/local后，独立C应用仅链接-lsnia，完整文件排序再次通过。保留/data/cs-batch-input.bin供查看；未重启Guest或重新分区。

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

## 2026-10-20：CPU 与 CSD 排序性能对比

- 新增 tests/read_sort_limit.cpp：Guest普通read读取同一256MiB文件，单线程std::sort全量降序排序后取前5；read前对该文件调用POSIX_FADV_DONTNEED请求丢弃Guest页缓存。
- 示例增加--existing只读复用数据文件，避免每轮重写；新增单调时钟计时，分别记录LOAD+sink、source+limit和总耗时。准备文件、extent查询、算子下载/激活及初始AFDM/请求分配不计时，运行中的workmem申请、spill与EOF回收计时。
- 测试脚本增加bench，自动编译、交替执行CPU/CSD共3轮，校验全部前5并保存分阶段TSV和日志。日志使用独立/tmp目录，避免共享results挂载目录的宿主写权限限制。
- 宿主GCC9、Guest GCC13编译通过；6次真实执行全部通过结果比对。Guest为8vCPU/8GiB，CPU使用256MiB数组，CSD保持64MiB workmem和SERIAL batch。
- CPU三轮总耗时10.446936/10.120032/10.004880秒，平均10.190616秒；CSD为61.348394/61.575849/61.105015秒，平均61.343086秒，CSD/CPU为6.02。
- CPU平均read/sort+limit为0.579679/9.610937秒；CSD平均LOAD+sink/source+limit为8.124240/53.218846秒。后者含完整归并、spill读取和回收，约占CSD总时间87%；源码中逐值遍历全部run的归并和不同内存预算是分析差异的重要背景，未据此把阶段时间直接解释为纯I/O带宽。
- 本次原始数据位于/tmp/snia-sort-limit.NsgtKw/timings.tsv，中文示例说明保留三轮结果与计时口径。

## 2026-10-20：统一64MiB预算与外部归并流程

### 修改

- 删除无限制全量内存基线 tests/read_sort_limit.cpp；普通路径改为独立 C 文件 code/csd-programs/examples/tests/read_sort_limit.c，不调用SNIA/NVMe。由设备示例CMake编译并动态链接同一workmem实现，脚本将兼容Guest的二进制与库传入/tmp/csd-sort-bench，避免Guest挂载范围导致额外源码复制或构建配置。
- 普通与CSD路径均按4MiB读入、生成64个降序run，workmem申请失败才淘汰编号最小的驻留run。两边共用无动态scratch分配的原地quicksort，较小分区递归限制栈深度；归并均逐值比较所有未耗尽run，每个spill run按256KiB缓冲，输出4MiB chunk并保留首5，读完EOF。
- 两边的64MiB总算法预算包括run、描述、归并缓冲、分配块头与四个输入输出/I/O缓冲。扣除约8.25MiB缓冲后，剩余容量按页向下对齐，CSD context和CPU工作池均限制在约55.75MiB；FEMU全局workmem和FDM配置不变。固定进程/库/线程管理开销不属于算法预算，未设置整个FEMU或Guest进程的RSS上限。
- 普通路径在输入目录创建临时文件，立即unlink，使用O_DIRECT读输入并读写spill，避免Guest页缓存绕过算法预算。计时包含文件I/O、run申请/排序/淘汰、全量归并、工作内存和临时文件回收；CSD保持内部LOAD/STORE、batch重入和EOF FREE路径。
- 示例局部输出结构增加run数、spill数、读写量和内存峰值；limit在EOF转交统计，batch脚本校验各次结果与I/O工作量一致及预算上限。公开SNIA头、33个API、插件与原生设备ABI不变，示例两端需同步重编。
- 中文示例/测试说明更新计时范围、预算范围、构建位置和256KiB的用途：它是读写块及归并缓冲，仍只有64个4MiB run，未增加run数。

### 验证与实测

- 共享C排序与std::sort独立比较，覆盖空输入、单值、插入排序边界、相等值、升降序和含正负数的散列序列；现有测试继续逐项比较完整多run归并流，并检查统计、驻留零I/O、按需spill、归并内存压力、EOF和提前停止。
- 同一256MiB文件、相同单线程/串行执行，交替测量3轮。CPU总延迟85.430560/85.177183/85.095577秒，平均85.234440秒；CSD为61.987511/61.805069/61.520932秒，平均61.771171秒。CPU/CSD为1.380，CSD延迟降低约27.5%。
- 两阶段CPU平均8.631567/76.602873秒，CSD平均8.458890/53.312280秒。两边每轮64个run，最终累计spill 54个，写入/回读各226492416字节（216MiB），CPU峰值66850264、CSD峰值66859416字节，均小于67108864。6次前5一致。
- 宿主5组SNIA与4组设备CTest通过，容器4组设备CTest通过，最终构建脚本正常退出；排序ASan/UBSan/LeakSanitizer通过（泄漏检查在沙箱外运行，避免ptrace限制）；临时文件关闭后数据分区使用量回到257MiB。
- 原始数据及日志保存在/tmp/snia-sort-limit.tvUcHU。此前10.19秒CPU全内存结果与64MiB CSD的6.02倍差距不属于相同预算/算法比较，新的结果取代该结论；归并仍占主要阶段时间，未把混合阶段时间解释成纯I/O带宽。

## 2026-10-20：细分计时与大根堆归并

### 修改

- 设备sort.cpp和普通read_sort_limit.c共用batch_sort.h中的固定64槽大根堆，缓存头部整数及run编号。初始化只遍历一次全部run，之后只推进堆顶对应run；堆和新增状态计入workmem。保留4MiB run、256KiB spill块、64MiB总算法预算与全量输出，未开启LIMIT提前结束。
- source保存“堆顶待更新”状态，LOAD/STORE重入后先完成更新再输出，跨输出chunk边界同样有效；缓存值不依赖可能因淘汰失效的run地址。EOF仍完成全部FREE并销毁状态。
- 算子记录sink/source累计墙钟、线程CPU与run排序墙钟时间；最后一个输出通过LIMIT交回Guest。普通路径同步统计文件读取、spill读写和释放时间，从阶段时间扣除I/O以报告工作时间。
- FEMU Guest插件增加SNIA_FEMU_PROFILE=1诊断开关，在close时输出E1、D0、D1、TRIM、F2次数及ioctl累计墙钟，默认关闭。内部计数采用原子操作，保持errno和原返回状态，不新增公开API、插件ABI或设备命令。
- bench自动启用诊断并收集stderr，增加profiles.tsv；中文文档说明计时的包含关系，E1与CSF执行不能重复相加。多CU仍由应用构造HYBRID依赖，不推测算子并行性；本阶段没有实现内部I/O预取。
- 说明现有LIMIT返回累计条数和输出count已足以让应用停止下一批；finished只表示source EOF和状态释放。提前停止还需显式释放剩余排序状态和spill，不能把LIMIT满足直接伪装成EOF；首批输出上限仍需后续单独支持。

### 验证与性能结论

- 在改归并前先新增计时并测一轮：CPU86.047857秒；CSD62.344995秒，其中source执行52.925500、线程CPU52.903354秒，run排序7.804513秒，D0/D1/TRIM合计1.102058秒。原归并明显受计算限制。
- 大根堆版交替执行CPU/CSD各3轮，CPU总延迟13.337534/13.470178/13.531890，平均13.446534秒；CSD为13.624559/13.289712/13.485604，平均13.466625秒。对比上一阶段三轮平均85.234440/61.771171秒，分别约6.34/4.59倍改善；现在两条路径基本相当。
- 新版CSD平均sink/source阶段8.492733/4.973892秒，算子累计执行8.024623/4.570028秒，source线程CPU4.567398秒，run排序7.795755秒。E1累计12.809140秒；内部D0/D1/TRIM/F2为0.217255/0.295964/0.126260/0.002170秒。E1减两类排序CSF执行约0.214489秒，仍包含LIMIT及其他命令成本。主要瓶颈已转为run排序，其次为堆归并。
- 两边均64个run、54个spill run，写入与回读各226492416字节，内存峰值CPU66850904、CSD66860056字节；6次前5一致，命令次数E1=2784、D0=928、D1=864、TRIM=864、F2=64。原始数据、profile TSV及原算法基线日志位于/tmp/snia-sort-limit.f3wFHU。
- 宿主5组SNIA及4组设备CTest通过。排序测试比较完整多run输出，覆盖跨块重入、输出chunk边界、内存压力淘汰、EOF与提前停止；补充64槽堆替换/删除、重复值、INT32极值检查及计时字段检查。生产传输测试覆盖诊断开关、命令分类、忽略Identify、失败请求计数和errno保持。排序ASan/UBSan/LeakSanitizer通过。

## 2026-10-20：完整排序后分批AVG，移除LIMIT示例

### 修改

- 删除设备limit.c及cs-example-limit.so，新增单文件avg.c，入口avg_int32，编译为cs-example-avg.so。Guest示例更名cs_batch_sort_avg.c/目标cs-batch-sort-avg；普通C基线更名read_sort_avg.c/目标read-sort-avg。同步CMake、脚本、项目及设备说明，移除当前文档中的LIMIT提前结束流程；历史记录保留。
- 输出AFDM中的average_result以int64 sum、uint64 count跨批次累计，再计算double average，finished仅跟随source EOF。初始结果清零，不把各批均值直接相加或取平均。256MiB int32的最坏总和绝对值不超过2^57；空输入以count=0区分，average暂置0，无SQL NULL语义。AVG返回累计count，不触发提前结束。
- CPU与CSD共用同一分批累加函数，第二阶段均为sort.source→AVG并消费完整输出。新增average_work墙钟和线程CPU计时；CPU source_work扣除AVG时间，保持与设备source入口相同口径。最终必须消费67,108,864个整数。
- 文件生成时独立累计参考总和并在真实示例末尾核对；bench交替测量三轮，严格核对每次总和、条数、均值、spill量和内存预算。仍使用大根堆、4MiB run、256KiB spill块、共享workmem及SERIAL，无自动多CU或预取。
- 设备测试继续逐项比较全部排序输出，AVG增加不等长批次[10,20]与[90]得到40的检查，覆盖空批次、空EOF、负数/INT32极值以及拒绝重复消费EOF；SNIA mock batch改用真实AVG并核对全部数据的sum/count/average和EOF回收。

### 验证与实测

- 宿主4组设备与5组SNIA CTest通过，容器4组设备CTest通过。容器及Guest同步重编，旧LIMIT设备SO和旧Guest/CPU临时二进制已删除；数据分区在运行结束后回到257MiB占用。排序与AVG的ASan/UBSan/LeakSanitizer检查通过。
- Guest真实示例重新生成相同伪随机数据，在2个extent上完整执行并通过生成时的独立总和参考。6次延迟测试结果均为count=67108864、sum=15688155813636、average=233771.73861318827。
- 三轮CPU总延迟13.827569/13.538000/13.739177，平均13.701582秒；CSD为13.388101/13.345029/13.465496，平均13.399542秒。CPU/CSD约1.023，两条路径仍为相近量级。
- 两阶段CPU平均8.779530/4.922052秒，CSD为8.383206/5.016335秒。AVG自身平均CPU0.054643、CSD0.059315秒；CSD sink/source执行7.984451/4.550438秒，run排序7.774153秒，E1累计12.797748秒，D0/D1/TRIM/F2为0.238663/0.228625/0.115047/0.002520秒。E1包含AVG执行，不可与三个CSF执行时间重复相加。
- 两边均64个run、54个spill run，写入及回读各226492416字节；CPU算法内存峰值66850920、CSD66860072字节，均小于64MiB。原始日志、timings.tsv及profiles.tsv位于/tmp/snia-sort-avg.GQq8lV。
