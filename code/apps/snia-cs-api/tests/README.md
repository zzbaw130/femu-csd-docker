# SNIA 功能测试

测试使用公开 cs.h 或设备插件接口，按实际检查的 API 功能命名。测试未启用时不依赖额外脚本语言。

| 文件 | CTest 用例 | 检查内容 |
| --- | --- | --- |
| common/cs_header_compatibility.cpp | csHeaderCompatibility | C++ 应用只包含 cs.h |
| common/cs_library_management.c | csLibraryManagementRouting | 注册/注销两种插件、发现、句柄路由及请求绑定 |
| common/cs_afdm_event_lifecycle.cpp | csAFDMEventAllocationFailure | AFDM、event、异步提交分配失败和资源回收 |
| femu/cs_discovery.c | csDiscoveryIdentify | 生产传输的 Identify、资源查询和打开失败 |
| femu/cs_csf_batch_scheduling.c | csCSFSchedulingAndBatch | 计算、AFDM、storage、event、三种 batch 模式及失败回收 |
| femu/cs_batch_scheduling_live.c | 手动 cs-batch-scheduling-live | Guest 真实设备并行、64位完成值及 spill |

common/mock_cs_backend.c 是第二种非NVMe设备；femu/mock_nvme_transport.c 在内存中模拟原生命令；femu/cs_batch_spill_scenarios.h 供模拟与真实设备复用跨批次 spill 场景。这些文件仅提供测试后端或场景，不是标准接口实现。

构建与运行：

```sh
bash tests/snia-cs-api-test.sh unit
bash tests/snia-cs-api-test.sh build
bash tests/snia-cs-api-test.sh smoke
```

unit 为宿主测试单独编译设备 SO，防止混用容器系统库版本。也可用 CMake 配置测试时设置 CSD_TEST_PROGRAM_DIR，指向本机编译的 cs-example-functions.so 所在目录。
