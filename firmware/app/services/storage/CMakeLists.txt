# services/storage/CMakeLists.txt
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/storage.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_time.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_config.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_seq.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_identity.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_timefloor.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_ctrllog.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_record.c
    ${CMAKE_CURRENT_LIST_DIR}/storage_m2.c
    ${CMAKE_CURRENT_LIST_DIR}/m3_selftest.c
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
