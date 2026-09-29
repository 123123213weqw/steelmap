/* XRT 单头文件的唯一实现编译单元(纯 C,与 C++ 分开编译)。
 * XRT_MODULE_ALL 由构建系统统一定义,保证与 xrt_transport.cpp 一致。
 */
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#define XRT_IMPLEMENTATION
#include <xrt.h>
