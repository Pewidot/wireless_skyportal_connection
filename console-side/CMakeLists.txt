cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(console-side)

# Re-apply our local patches to the freshly-fetched managed components
# (idempotent; runs after the component manager fetches them, before the build).
find_program(PATCH_PY NAMES python3 python)
if(PATCH_PY)
  execute_process(COMMAND ${PATCH_PY}
                  ${CMAKE_CURRENT_LIST_DIR}/../shared/patch_components.py
                  ${CMAKE_CURRENT_LIST_DIR})
endif()
