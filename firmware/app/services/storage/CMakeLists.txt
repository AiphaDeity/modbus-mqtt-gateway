# services/storage/CMakeLists.txt
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/storage.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_time.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_config.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_seq.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_identity.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_timefloor.c
    # storage_record.c — TF 卡记录持久化（待 M2 对齐记录格式后实现）
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
