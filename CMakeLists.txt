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

# LANGUAGES 必须含 C：GResource 生成的 awcc.gresource.c 是 C 源码（见下面「前端资源」一节）。
project(
  awcc
  LANGUAGES C CXX
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

# ===================== GTK4 =====================
# 前端用 GTK4 C API + libadwaita（见 DESIGN.md 第二节）：系统库，走 pkg-config。
# 不引 gtkmm（它虽是官方 C++ 绑定）：官方文档与示例全围绕 C API，且 libadwaita
# 没有 C++ 绑定（本机与发行版仓库都没有 libadwaitamm）。
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)

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

# ===================== 前端资源 =====================
# .ui 与 CSS 用 GtkBuilder + GResource 打进二进制（见 DESIGN.md 第二节）。资源名与 alias
# 见 src/ui/awcc.gresource.xml —— 那里以仓库根为 --sourcedir，所以 DEPENDS 用相对仓库根的路径。
find_program(GLIB_COMPILE_RESOURCES NAMES glib-compile-resources REQUIRED)
set(AWCC_GRESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/awcc.gresource.c)
add_custom_command(
  OUTPUT ${AWCC_GRESOURCE_C}
  COMMAND
    ${GLIB_COMPILE_RESOURCES} --generate-source --target=${AWCC_GRESOURCE_C}
    --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/awcc.gresource.xml
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/awcc.gresource.xml
          ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/awcc.ui
          ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/style.css
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/modes/quiteMode.png
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/modes/balancedMode.png
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/modes/performanceMode.png
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/modes/batteryMode.png
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/modes/gMode.png
  VERBATIM)

# ===================== 国际化 =====================
# 源串写英文，译文放 po/*.po，运行期 gettext 按 LANG / LC_MESSAGES 自动选（见 main.cpp 的
# setupI18n 与 DESIGN.md 第二节）。从构建树里跑时用 build/locale 下的 .mo，装到系统后走
# /usr/share/locale；两者都编进二进制，运行期择一。
find_package(Intl REQUIRED)
find_program(MSGFMT NAMES msgfmt REQUIRED)
find_program(XGETTEXT NAMES xgettext REQUIRED)

set(AWCC_LOCALEDIR_INSTALL
    "/usr/share/locale"
    CACHE PATH "安装后的 locale 目录")
set(AWCC_LOCALEDIR_BUILD "${CMAKE_BINARY_DIR}/locale")

file(GLOB AWCC_PO_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/po/*.po)
set(AWCC_MO_FILES "")
foreach(po_file ${AWCC_PO_FILES})
  get_filename_component(lang ${po_file} NAME_WE)
  set(mo_file ${AWCC_LOCALEDIR_BUILD}/${lang}/LC_MESSAGES/awcc.mo)
  add_custom_command(
    OUTPUT ${mo_file}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${AWCC_LOCALEDIR_BUILD}/${lang}/LC_MESSAGES
    COMMAND ${MSGFMT} --check-format -o ${mo_file} ${po_file}
    DEPENDS ${po_file}
    VERBATIM)
  list(APPEND AWCC_MO_FILES ${mo_file})
  install(FILES ${mo_file}
          DESTINATION ${AWCC_LOCALEDIR_INSTALL}/${lang}/LC_MESSAGES
          RENAME awcc.mo
          COMPONENT awcc)
endforeach()

add_custom_target(translations ALL DEPENDS ${AWCC_MO_FILES})

# 改了源码或 .ui 里的串之后，用 `ninja -C build pot` 刷新模板，再 msgmerge 进各 .po
add_custom_target(
  pot
  COMMAND
    # 传相对路径：xgettext 会把源码位置写进 pot，用绝对路径会把本机目录泄露出去
    ${XGETTEXT} --language=C++ --keyword=_ --keyword=N_ --from-code=UTF-8
    --package-name=awcc --output=po/awcc.pot src/ui/Ui.cpp
  COMMAND
    ${XGETTEXT} --language=Glade --from-code=UTF-8 --package-name=awcc -j
    --output=po/awcc.pot src/ui/awcc.ui
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "生成 po/awcc.pot")

# ===================== App =====================
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(${APP_NAME} ${SOURCES} ${AWCC_GRESOURCE_C})

add_dependencies(${APP_NAME} translations)

target_compile_definitions(
  ${APP_NAME}
  PRIVATE GETTEXT_PACKAGE="awcc"
          AWCC_LOCALEDIR_BUILD="${AWCC_LOCALEDIR_BUILD}"
          AWCC_LOCALEDIR_INSTALL="${AWCC_LOCALEDIR_INSTALL}")

target_include_directories(${APP_NAME} PRIVATE include)

target_link_libraries(
  ${APP_NAME}
  PRIVATE PkgConfig::GTK4
          PkgConfig::ADWAITA
          loguru::loguru
          nlohmann_json::nlohmann_json
          usb-1.0
          libevdev::libevdev)

if(UNIX AND NOT APPLE)
  target_link_libraries(${APP_NAME} PRIVATE dl pthread)
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

install(TARGETS ${APP_NAME} RUNTIME DESTINATION /usr/bin COMPONENT awcc)

install(
  DIRECTORY app/
  DESTINATION /usr/share/applications
  COMPONENT awcc
  FILES_MATCHING
  PATTERN "*.desktop")

install(
  DIRECTORY app/
  DESTINATION /usr/share/icons
  COMPONENT awcc
  FILES_MATCHING
  PATTERN "*.png")

install(FILES app/70-awcc.rules DESTINATION ${UDEV_RULES_DIR} COMPONENT awcc)

if(OPENRC)
  message(STATUS "Configuring for OpenRC installation")
  install(
    FILES app/awccd.initd
    DESTINATION /etc/init.d
    RENAME awccd
    COMPONENT awcc
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

  install(FILES app/awccd.service DESTINATION ${SYSTEMD_UNIT_DIR} COMPONENT awcc)
endif()

install(FILES database.json DESTINATION ${AWCC_CONFIG_DIR} COMPONENT awcc)

# 说明：deb / rpm 的包不做在 CMake 里。Debian 用 debian/ + dpkg-buildpackage、RPM 用
# packaging/rpm/awcc.spec + rpmbuild（各自的官方方案），由 scripts/package.sh 驱动。
# 上面 install() 里的 COMPONENT awcc 是给这两个方案用的：只装我们自己那一个组件，
# 免得把 FetchContent 拉来的依赖（libusb 的头文件与静态库）也装进系统。
