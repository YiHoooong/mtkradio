# SPDX-License-Identifier: GPL-2.0-only
# Standalone user-mode MediaTek MT7922 / MT7921 (Connac2) 802.11 / AWDL radio daemon.
cmake_minimum_required(VERSION 3.20)
project(mtkradio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
  set(MTK_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(MTK_OPTS /W3 /EHsc -Wno-address-of-packed-member -Wno-unused-function)
  else()
    set(MTK_OPTS /W3 /EHsc /wd4005)
  endif()

  add_executable(mtkradio
    src/mtk_main.cpp
    src/mtk_dev.cpp
    src/mtk_dma.cpp
    src/common/proxy.cpp
    src/common/plat_win.cpp
  )
  target_include_directories(mtkradio PRIVATE src src/common)
  target_compile_definitions(mtkradio PRIVATE ${MTK_DEFS})
  target_compile_options(mtkradio PRIVATE ${MTK_OPTS})
  target_link_libraries(mtkradio PRIVATE setupapi winmm advapi32)
else()
  add_executable(mtkradio_linux
    src/mtk_linux_test.cpp
    src/mtk_dev.cpp
    src/mtk_vfio.cpp
  )
  target_include_directories(mtkradio_linux PRIVATE src src/common)
  target_compile_options(mtkradio_linux PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(mtkradio_linux PRIVATE pthread)
endif()
