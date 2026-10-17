# OTA 升级组件（A/B 双分区：下载 → 校验镜像头 → 自检 → 切换启动分区/回滚）
#
# 依赖核对（按 src/ota_update.c 的 include 逐项确认）：
#   app_update       esp_ota_ops.h    分区状态查询/标记有效/回滚重启
#   esp_https_ota    esp_https_ota.h  流式下载并写入备用分区（含 TLS）
#   esp_http_client  esp_http_client.h esp_http_client_config_t（esp_https_ota.h 亦包含，显式声明）
#   esp_app_format   esp_app_desc.h   镜像描述（magic/项目名/版本）
#   esp_partition    esp_partition.h  esp_partition_t（esp_ota_get_running_partition 返回类型）
#   mbedtls          esp_crt_bundle.h HTTPS 服务端证书包校验
#                    （仅 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y 时实际使用）
idf_component_register(
    SRCS "src/ota_update.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_https_ota esp_http_client esp_app_format esp_partition mbedtls)
