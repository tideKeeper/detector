#include "plthook.h"
#include <cstdio>
#include <cstdarg>
#include <dlfcn.h>

// 保存原 printf 函数指针
typedef int (*printf_t)(const char*, ...);
printf_t g_original_printf = nullptr;

int hooked_printf(const char* fmt, ...) {
    // 第一步：执行自定义拦截逻辑
    printf("[HOOK 拦截] 捕获到 printf 调用\n");
    
    // 第二步：透传参数，调用原始 printf 实现
    va_list args;
    va_start(args, fmt);
    // 直接通过原地址调用，绕过 PLT，不会触发递归
    int ret = g_original_printf(fmt, args);
    va_end(args);

    // 第三步：返回原函数的执行结果
    return ret;
}

int main() {
    // 先加载动态库，确保它在进程中
    void* lib_handle = dlopen("./libtest.so", RTLD_LAZY);
    if (!lib_handle) {
        printf("加载动态库失败: %s\n", dlerror());
        return 1;
    }

    // 创建针对 libtest.so 的 PLT Hook
    auto hook = PLTHook::Create("libtest.so");
    if (!hook) {
        printf("创建 Hook 失败: %s\n", PLTHook::GetLastError().c_str());
        return 1;
    }

    // 替换 printf 函数
    int ret = hook->ReplaceFunction("printf", (void*)hooked_printf, (void**)&g_original_printf);
    if (ret != PLTHook::SUCCESS) {
        printf("替换函数失败: %s\n", PLTHook::GetLastError().c_str());
        return 1;
    }

    // 获取 SimpleAdd 函数指针并调用
    typedef int (*add_func)(int, int);
    add_func SimpleAdd = (add_func)dlsym(lib_handle, "SimpleAdd");
    printf("=== 调用动态库函数 ===\n");
    int result = SimpleAdd(3, 5);
    printf("返回结果: %d\n", result);

    dlclose(lib_handle);
    return 0;
}