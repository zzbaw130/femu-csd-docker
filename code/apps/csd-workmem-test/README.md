# Guest 工作内存集成测试

本应用在 Guest vCPU 上运行，只通过 SNIA API 调用设备 CSF；不在 Guest 链接设备分配器。

设备侧源码位于 `code/csd-programs/csd-workmem`，公共库和两个探针 .so 编译至 `csd-programs/build`，由容器内 FEMU CU 加载。需要 4 CU，原有命令即可执行，不写 namespace 数据。

在 Guest 中：

```bash
make -C /home/femu/apps/csd-workmem-test BUILD_DIR=/tmp/csd-workmem-test \
    SDK_BUILD_DIR=/tmp/sina-cs-api-guest
sudo env SINA_CS_FEMU_DEVICES=/dev/nvme0n1:67108864:4 \
    /tmp/csd-workmem-test/csd-workmem-test /dev/nvme0n1
```

测试预留程序索引 63000..63005。四个独立程序实例覆盖两个不同 .so，各申请工作池的 3/8。AFDM 屏障确保所有请求完成分配后才开始释放，预期两个成功、两个 ENOMEM，峰值不超过总容量，结束时占用恢复为零；重复两轮。探针有 5 秒等待超时，配置不足 4 CU 会报告执行错误。

测试成功后回收 AFDM、batch、程序及 API 句柄。设备工作区超额由探针正常记录为 ENOMEM，不等同于 NVMe 请求失败。
