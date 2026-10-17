# 配置目录

- `config.gz`：设备内核配置，来自 `/proc/config.gz`，不含用户数据。
- `Module.symvers`：从设备预编译模块 ELF 元数据导出的 MODVERSIONS CRC。

不要在本目录存放厂商 `.ko`、固件、设备序列号、完整日志或用户数据。
