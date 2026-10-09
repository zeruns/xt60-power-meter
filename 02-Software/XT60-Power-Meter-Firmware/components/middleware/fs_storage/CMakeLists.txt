# 资源文件系统组件：挂载 partitions.csv 中的 storage（SPIFFS）分区
idf_component_register(
    SRCS "src/fs_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES spiffs)

# 把 spiffs_image/ 目录打成 SPIFFS 镜像并随 idf.py flash 一起烧入
# storage 分区（与 partitions.csv 的 Name 对应）。
# 字体等资源放 spiffs_image/ 下，不占用 app 分区程序空间。
spiffs_create_partition_image(storage spiffs_image FLASH_IN_PROJECT)
