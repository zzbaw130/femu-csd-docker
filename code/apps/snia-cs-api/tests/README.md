# SNIA 功能测试

测试使用公开 cs.h 或设备插件接口，按实际检查的 API 功能命名。测试未启用时不依赖额外脚本语言。

| 文件 | CTest 用例 | 检查内容 |
| --- | --- | --- |
| common/cs_header_compatibility.cpp | csHeaderCompatibility | C++ 应用只包含 cs.h |
| common/cs_library_management.c | csLibraryManagementRouting | 注册/注销两种插件、发现、句柄路由及请求绑定 |
| common/cs_afdm_event_lifecycle.cpp | csAFDMEventAllocationFailure | AFDM、event、异步提交分配失败和资源回收 |
| femu/cs_discovery.c | csDiscoveryIdentify | 生产传输的Identify、资源查询、打开失败，以及可选命令计时分类/关闭/错误码保持 |
| femu/cs_csf_batch_scheduling.c | csCSFSchedulingAndBatch | AFDM、storage、event、batch依赖、排序跨批次spill、完整平均值聚合及EOF回收 |
| csd-programs/examples/tests/read_sort_avg.c（设备示例目录） | 手动 read-sort-avg | 独立 C 文件读写、64MiB总算法预算、按需spill、全量归并后求平均值，与CSD比较延迟 |
| examples/cs_batch_sort_avg.c（项目 examples/） | 手动 cs-batch-sort-avg | Guest真实设备完整文件排序、全量均值与自动回收 |

common/mock_cs_backend.c 是第二种非NVMe设备；femu/mock_nvme_transport.c 在内存中模拟原生命令。这些文件仅提供测试后端或场景，不是标准接口实现。

构建与运行：

```sh
bash tests/snia-cs-api-test.sh unit
bash tests/snia-cs-api-test.sh build
bash tests/snia-cs-api-test.sh smoke
bash tests/snia-cs-api-test.sh bench
```

bench 复用已有256MiB示例文件，交替测量3轮并校验结果，输出分阶段耗时、profiles.tsv细分计时和临时日志目录。计时范围、直接 I/O 及内存预算见[示例说明](../examples/README.md#计时与性能测试)。

unit 为宿主测试单独编译设备 SO，防止混用容器系统库版本。也可用 CMake 配置测试时设置 CSD_TEST_PROGRAM_DIR，指向本机编译的 cs-example-sort.so、cs-example-avg.so 所在目录。
