cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME cloudsync)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)
# Resolve the real source directory (in case we're accessed via symlink)
get_filename_component(CLOUDSYNC_REAL_DIR "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
get_filename_component(CLOUDSYNC_PARENT_DIR "${CLOUDSYNC_REAL_DIR}/.." REALPATH)

include_directories(${CLOUDSYNC_PARENT_DIR})

# CloudSync core sources (shared with SQLite/PostgreSQL)
set(CLOUDSYNC_CORE_SOURCES
    ${CLOUDSYNC_PARENT_DIR}/cloudsync.c
    ${CLOUDSYNC_PARENT_DIR}/dbutils.c
    ${CLOUDSYNC_PARENT_DIR}/lz4.c
    ${CLOUDSYNC_PARENT_DIR}/pk.c
    ${CLOUDSYNC_PARENT_DIR}/utils.c
)

# DuckDB-specific sources
set(EXTENSION_SOURCES
    cloudsync_duckdb.cpp
    database_duckdb.cpp
    sql_duckdb.c
    ${CLOUDSYNC_CORE_SOURCES}
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link Security framework on macOS (for SecRandomCopyBytes in utils.c)
if(APPLE)
    find_library(SECURITY_FRAMEWORK Security)
    if(SECURITY_FRAMEWORK)
        target_link_libraries(${LOADABLE_EXTENSION_NAME} ${SECURITY_FRAMEWORK})
        target_link_libraries(${EXTENSION_NAME} ${SECURITY_FRAMEWORK})
    endif()
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
