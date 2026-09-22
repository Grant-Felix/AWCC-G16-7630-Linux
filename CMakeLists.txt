cmake_minimum_required(VERSION 3.16)

# 本 fork 的版本号方案（见 ADAPTATION.md 第六节）：日期式 v<YY>.<M>.<D>-<当天第 x 次打包>，
# 如 v26.9.22-1 是 2026-09-22 当天第 1 次打包。版本串的唯一出处是 git tag，这里取已有的
# 日期式 tag 里版本最大的那个，于是每次发布都不必改本文件——改了就每次与上游 rebase 都冲突。
# CI 在 tag 上 checkout，显式传 -DAWCC_VERSION=<串>，不查 tag。
set(AWCC_VERSION
    ""
    CACHE STRING "完整版本串，如 26.9.22-1；留空则取版本最大的日期式 git tag")

if(NOT AWCC_VERSION)
  # 用 git tag 而不是 git describe：同一天多次打包会把多个 tag 打在同一个提交上（这正是
  # 「第 x 次打包」的语义），实测 describe 在那种情况下会挑到前一天的 tag；按 version:refname
  # 排序取最大才是「最新一次打包」。
  execute_process(
    COMMAND git tag -l --merged HEAD --sort=-v:refname
            "v[0-9]*.[0-9]*.[0-9]*-*"
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE AWCC_DATE_TAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE AWCC_GIT_TAG_RESULT)
  if(AWCC_GIT_TAG_RESULT EQUAL 0 AND AWCC_DATE_TAGS)
    string(REPLACE "\n" ";" AWCC_DATE_TAGS "${AWCC_DATE_TAGS}")
    list(GET AWCC_DATE_TAGS 0 AWCC_VERSION)
    string(REGEX REPLACE "^v" "" AWCC_VERSION "${AWCC_VERSION}")
  else()
    # 没有日期式 tag（例如从 tarball 构建）时的占位，绝不能当成已发布版本
    set(AWCC_VERSION "0.0.0-0")
  endif()
endif()

# project(VERSION) 只收纯数字 major.minor.patch[.tweak]，装不下「-序号」，故只把数字部分喂它，
# 完整串留给 VERSION 宏（awcc help / device-info 与 GUI 标题用）。
string(REGEX REPLACE "-[0-9]+$" "" AWCC_VERSION_NUMERIC "${AWCC_VERSION}")

project(
  awcc
  LANGUAGES CXX
  VERSION ${AWCC_VERSION_NUMERIC})

message(STATUS "awcc 版本串：${AWCC_VERSION}（CMake project VERSION ${PROJECT_VERSION}）")

set(APP_NAME awcc)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF)

# if(NOT CMAKE_BUILD_TYPE)
set(CMAKE_BUILD_TYPE
    Release
    CACHE STRING "Build type" FORCE)
# endif()

add_compile_definitions(VERSION="${AWCC_VERSION}")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX
      "/usr"
      CACHE PATH "Install path prefix" FORCE)
endif()

# compile_commands.json symlink
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  add_custom_target(
    link_compile_commands ALL
    COMMAND
      ${CMAKE_COMMAND} -E create_symlink
      ${CMAKE_BINARY_DIR}/compile_commands.json
      ${CMAKE_SOURCE_DIR}/compile_commands.json
    BYPRODUCTS ${CMAKE_SOURCE_DIR}/compile_commands.json)
endif()

include(FetchContent)
find_package(OpenGL REQUIRED)

if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
endif()

# ===================== Loguru =====================
FetchContent_Declare(
  loguru
  GIT_REPOSITORY https://github.com/emilk/loguru
  GIT_TAG master
  GIT_SHALLOW TRUE)
set(LOGURU_WITH_STREAMS
    TRUE
    CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(loguru)

# ===================== nlohmann_json =====================
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG master
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(json)

# ===================== GLFW =====================
set(GLFW_BUILD_EXAMPLES
    OFF
    CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS
    OFF
    CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS
    OFF
    CACHE BOOL "" FORCE)
set(GLFW_INSTALL
    OFF
    CACHE BOOL "" FORCE)

FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG master
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

# ===================== ImGui =====================
# 本 fork 修改（2026-09-21，详见 ADAPTATION.md）：上游这里是 GIT_TAG master。imgui master 是
# 1.93 WIP，2026-09 那批提交里取色器（ColorEdit 的弹窗）在本机渲染成全黑，所以钉回 v1.19.0
# 发布时（2026-06-04）CI 取到的那个提交。钉 SHA 不能用浅克隆，故 GIT_SHALLOW 关掉。
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG 6acba3b47d2ac4c7bb5ffb6ab04bcd896b3d3658
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC)
target_sources(
  imgui
  PRIVATE ${imgui_SOURCE_DIR}/imgui.cpp
          ${imgui_SOURCE_DIR}/imgui_draw.cpp
          ${imgui_SOURCE_DIR}/imgui_tables.cpp
          ${imgui_SOURCE_DIR}/imgui_widgets.cpp
          ${imgui_SOURCE_DIR}/imgui_demo.cpp
          ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
          ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)

target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR}
                                        ${imgui_SOURCE_DIR}/backends)

target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

if(UNIX AND NOT APPLE)
  target_link_libraries(imgui PUBLIC X11::X11 dl pthread)
endif()

# ===================== libusb =====================
set(LIBUSB_BUILD_SHARED
    OFF
    CACHE BOOL "" FORCE)
set(LIBUSB_BUILD_STATIC
    ON
    CACHE BOOL "" FORCE)

FetchContent_Declare(
  libusb
  GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
  GIT_TAG main
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(libusb)

# ===================== libevdev ============================================
include(ExternalProject)

ExternalProject_Add(
  libevdev
  GIT_REPOSITORY https://gitlab.freedesktop.org/libevdev/libevdev.git
  GIT_TAG master
  CONFIGURE_COMMAND
    meson setup <BINARY_DIR> <SOURCE_DIR> --default-library=static
    -Ddocumentation=disabled -Dtests=disabled -Dtools=disabled
  BUILD_COMMAND meson compile -C <BINARY_DIR>
  INSTALL_COMMAND "" # CMAKE NEEDS A INSTALL COMMAND or it will run make
  BUILD_BYPRODUCTS <BINARY_DIR>/libevdev.a)

add_library(libevdev::libevdev STATIC IMPORTED)

ExternalProject_Get_Property(libevdev source_dir binary_dir)

set_target_properties(
  libevdev::libevdev PROPERTIES IMPORTED_LOCATION ${binary_dir}/libevdev.a
                                INTERFACE_INCLUDE_DIRECTORIES ${source_dir})

add_dependencies(libevdev::libevdev libevdev)

# ===================== stb =====================
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG master
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(stb)

# ===================== App =====================
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(${APP_NAME} ${SOURCES})

target_include_directories(${APP_NAME} PRIVATE include ${stb_SOURCE_DIR})

target_link_libraries(
  ${APP_NAME}
  PRIVATE glfw
          imgui
          OpenGL::GL
          loguru::loguru
          nlohmann_json::nlohmann_json
          usb-1.0
          libevdev::libevdev)

if(UNIX AND NOT APPLE)
  target_link_libraries(${APP_NAME} PRIVATE X11::X11 dl pthread)
endif()

# ===================== Install =====================
set(UDEV_RULES_DIR
    "/etc/udev/rules.d"
    CACHE PATH "Directory for udev rules")
set(SYSTEMD_UNIT_DIR
    "/etc/systemd/system"
    CACHE PATH "Directory for systemd service files")
set(AWCC_CONFIG_DIR
    "/etc/awcc"
    CACHE PATH "Directory for configuration files")

install(TARGETS ${APP_NAME} RUNTIME DESTINATION /usr/bin)

install(
  DIRECTORY app/
  DESTINATION /usr/share/applications
  FILES_MATCHING
  PATTERN "*.desktop")

install(
  DIRECTORY app/
  DESTINATION /usr/share/icons
  FILES_MATCHING
  PATTERN "*.png")

install(FILES app/70-awcc.rules DESTINATION ${UDEV_RULES_DIR})

if(OPENRC)
  message(STATUS "Configuring for OpenRC installation")
  install(
    FILES app/awccd.initd
    DESTINATION /etc/init.d
    RENAME awccd
    PERMISSIONS
      OWNER_READ
      OWNER_WRITE
      OWNER_EXECUTE
      GROUP_READ
      GROUP_EXECUTE
      WORLD_READ
      WORLD_EXECUTE)
else()
  message(STATUS "Configuring for systemd installation")
  set(SYSTEMD_UNIT_DIR
      "/etc/systemd/system"
      CACHE PATH "Directory for systemd service files")

  install(FILES app/awccd.service DESTINATION ${SYSTEMD_UNIT_DIR})
endif()

install(FILES database.json DESTINATION ${AWCC_CONFIG_DIR})
