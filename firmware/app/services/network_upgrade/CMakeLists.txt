# services/network_upgrade/CMakeLists.txt
#
# M2 网络升级入口：MQTT 通知接收 + HTTP 分块下载 + 升级执行任务 + 状态上报
# 依赖 M4 升级框架（upgrade_entry/upgrade_quiesce/upgrade_stage）和 MQTT 客户端
#
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/network_upgrade.c
    ${CMAKE_CURRENT_LIST_DIR}/network_upgrade_downloader.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
