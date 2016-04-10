cmake_minimum_required(VERSION 3.1.0)
cmake_policy(SET CMP0054 NEW)

project(libatbus)

# 准备下载依赖组件
include ("${CMAKE_SOURCE_DIR}/3rd_party/c_cpp_utils/libc_cpp_utils.prepare.cmake")

########################################################################
# CMake 模块 (递归包含模块, 带颜色输出模块, 平台检测模块)
set (PROJECT_CMAKE_MODULE_DIR "${3RD_PARTY_C_CPP_UTILS_PKG_DIR}/project/cmake")
list(APPEND CMAKE_MODULE_PATH "${3RD_PARTY_C_CPP_UTILS_PKG_DIR}/modules")


include("${PROJECT_CMAKE_MODULE_DIR}/modules/IncludeDirectoryRecurse.cmake")
include("${PROJECT_CMAKE_MODULE_DIR}/modules/EchoWithColor.cmake")
include("${PROJECT_CMAKE_MODULE_DIR}/modules/FindConfigurePackage.cmake")

include("${PROJECT_SOURCE_DIR}/project/cmake/ProjectBuildOption.cmake")
include("${PROJECT_CMAKE_MODULE_DIR}/FindPlatform.cmake")
include("${PROJECT_CMAKE_MODULE_DIR}/ProjectTools.cmake")


#####################################################################
# 导入编译器和编译选项配置
include("${PROJECT_CMAKE_MODULE_DIR}/CompilerOption.cmake")
include("${PROJECT_CMAKE_MODULE_DIR}/TargetOption.cmake")
EchoWithColor(COLOR GREEN "-- Build Type: ${CMAKE_BUILD_TYPE}")

########################################################################
# 导入项目配置
## 导入所有 macro 定义
include_macro_recurse("${PROJECT_SOURCE_DIR}")

## 导入所有工程项目
add_project_recurse("${PROJECT_SOURCE_DIR}")
