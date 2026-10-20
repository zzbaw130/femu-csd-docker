# 开发阶段与验证记录

分支 pg_sina_csd_dev，基线 sina-femu-csd（f5a7df9dd）。每个可验收阶段单独提交。旧 pg_csd_dev 仅作实现参考，不移植其 Python 代理、AFDM 工作区或 FEMU SQL runtime。

## 阶段0：PG扩展、静态映射与共享heap通路（完成）

- 新增CMake构建的PG12扩展pg_sina_csd。sina_csd.prepare(regclass,device)检查持久化heap、visible/frozen覆盖，等待PG checkpoint，再读取FIEMAP。记录物理列布局、relfilenode、Guest boot ID、namespace/LBA几何和连续的全表extent映射，保存在扩展自己的三张表中。支持分区偏移和文件分段；无常驻连接或守护会话。
- Guest工具pg-sina-csd-check链接libpq和libsnia，使用标准API构建SERIAL LOAD→heap_count batch；复用描述并逐块更新，原始heap不经过工具缓冲，仅读回小型计数结果。原生PG COUNT独立核对结果，本阶段不作为性能比较。
- 设备heap_count.c只校验和解析PG12/x86-64/8KiB页，不接触文件、NVMe或PG规划信息。MR输入输出分离；cparam1表示物理列数，cparam2表示有效字节，正确处理尾块。无跨调用私有状态，统一链接workmem但无动态工作区。
- 新增宿主tests/pg-sina-csd-test.sh的build/prepare/check/sql入口。数据库schema名sina_csd避开PostgreSQL保留的pg_前缀；扩展/分支名称保留用户指定拼写。
- 当前Guest原先没有PG安装或数据集；通过既有脚本编译安装PG12.22和pg_visibility，恢复用户已有tpch_s1归档并启动数据库。复用现有80%数据分区，保留原256MiB排序文件；不重新分区、不重启FEMU。
- 宿主6项设备CTest通过；Guest扩展在-Werror下编译安装成功，真实PG头文件布局检查通过。设备count测试覆盖页/tuple越界、未冻结与未visible页、无效输入长度和非tuple line pointer；ASan/UBSan通过。检查环境不支持LeakSanitizer的ptrace模式，本无动态分配的测试运行时关闭detect_leaks。
- 八张表真实设备行数与PG完全一致：region=5、nation=25、supplier=10000、customer=150000、part=200000、partsupp=800000、orders=1500000、lineitem=6001215。lineitem映射112503页、12个extent，4MiB块共220次batch，内部读取921624576字节。
- 24KiB块读取part通过：4097页、1366个chunk、200000行；验证块大小无关和最后的不满块。SQL测试拒绝未冻结表，冻结后保存schema/extent；真实设备验证含NULL的3行表与空表，测试结束删除临时表和映射。

边界：当前SQL尚未自动卸载；只有准备函数和独立诊断工具。程序索引500供诊断独占，暂不支持跨backend并发索引管理；工具sudo访问NVMe，不给PG服务增加设备权限。准备后的实验表保持不变，设备/文件/schema变化后重新准备。

## 后续阶段

1. 从PG表达式树选择可支持的Scan/Filter/Aggregate路径，接入Q6 CustomScan、SNIA batch执行、精确numeric结果及错误/取消回收；同时落实PG请求访问NVMe的权限路径，不默认引入常驻代理。
2. 扩展动态表达式和pipeline编排，加入Q1分组、所需列裁剪、批处理、多CU局部结果合并；按实际测量决定SIMD和预取。
3. 加入Sort/TopN和高基数聚合的sink/source边界，以workmem保存私有状态，通过NEED_STORE/LOAD/FREE恢复协议完成spill。
4. 加入Hash Join的build/probe和分区spill，覆盖Q14，再逐步增加所需连接、字符串与子查询语义。
5. 逐条TPC-H正确性回归与性能分解；不能用少数Query或整数示例代表完整SQL支持，也不能把宿主CPU模拟结果解释为真实SSD性能。

## 执行路径与统一冷启动（开发中）

- 新增pg_sina_csd.exe_path：host为默认值，全部采用PG原生计划；csd优先采用已经实现且语义匹配的设备计划，不支持的节点或表达式保留PG路径。后续auto选项由规划器按成本选择；成本模型尚未实现前不开放auto，避免把固定选择误称为自动规划。
- Q1/Q6作为第一批端到端卸载对象：设备端完成扫描、过滤、表达式、聚合，Q1还包括排序与LIMIT。先从PG表达式树编排，不能按SQL文本或Query编号硬编码。
- 所有TPC-H测量统一冷启动：每条被测语句之前停止PG（清空shared buffers）、同步写入并清除Guest OS page cache，再启动PG。测试期间关闭autovacuum，避免后台扫描干扰。不能只drop_caches就把PG缓存仍热的执行称为冷启动。
- host与csd采用同样的冷启动流程；设备每次查询新建AFDM和工作上下文并完整回收，不复用上次查询的输入。FEMU模拟NVM的数据字节本身是存储介质，不是需要清除的文件缓存。
- 普通PG服务重启且表文件保持不变时复用准备结果；Guest/FEMU重启、数据重新恢复或文件/schema变化才重新prepare。不会为每次冷启动重复FIEMAP。
- 后续按聚合→Sort/TopN/高基数聚合spill→Hash Join/分区spill→更多连接/字符串/子查询推进，直至22条TPC-H正确性全部通过；每阶段记录具体算子、回退范围与冷启动性能，不能把部分Query通过记为完整支持。
