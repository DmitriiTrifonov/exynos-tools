cmake_minimum_required(VERSION 3.16)
project(exynos_tools C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

# Vulkan headers (>= 1.3, must include vulkan/vk_layer.h). The NDK's own copy may lack
# vk_layer.h; pass -DEXYNOS_VULKAN_HEADERS_PATH=<Vulkan-Headers>/include in that case.
set(EXYNOS_VULKAN_HEADERS_PATH "" CACHE PATH "Directory containing vulkan/vulkan.h and vulkan/vk_layer.h")
if(EXYNOS_VULKAN_HEADERS_PATH)
  include_directories(BEFORE ${EXYNOS_VULKAN_HEADERS_PATH})
endif()

add_library(VkLayer_exynos_tools SHARED src/layer/layer.cpp src/layer/bc_caps.cpp src/layer/bc_image.cpp)
target_compile_options(VkLayer_exynos_tools PRIVATE -Wall -Wextra)
if(ANDROID)
  target_link_libraries(VkLayer_exynos_tools PRIVATE log)
  target_link_options(VkLayer_exynos_tools PRIVATE -Wl,--strip-all)  # debug info is not needed on the phone
endif()
# Android loads layers named lib*.so from the app's lib dir; keep the standard name.
set_target_properties(VkLayer_exynos_tools PROPERTIES OUTPUT_NAME "VkLayer_exynos_tools")

configure_file(layer/VkLayer_exynos_tools.json
               ${CMAKE_BINARY_DIR}/VkLayer_exynos_tools.json COPYONLY)

# Probe tool: links the system Vulkan loader.
find_library(VULKAN_LIB NAMES vulkan)
if(VULKAN_LIB)
  add_executable(vk_probe tests/vk_probe.c)
  target_link_libraries(vk_probe PRIVATE ${VULKAN_LIB} ${CMAKE_DL_LIBS})
  add_executable(bc_upload_test tests/bc_upload_test.c)
  target_link_libraries(bc_upload_test PRIVATE ${VULKAN_LIB} ${CMAKE_DL_LIBS} m)
  # bcdec's signed BC4/BC5 path casts to plain `char`, which is unsigned on arm64 Linux/Android.
  target_compile_options(bc_upload_test PRIVATE -fsigned-char)
  add_executable(bc_bench tests/bc_bench.c)
  target_link_libraries(bc_bench PRIVATE ${VULKAN_LIB} ${CMAKE_DL_LIBS} m)
else()
  message(WARNING "libvulkan not found; vk_probe/bc_upload_test/bc_bench not built")
endif()
