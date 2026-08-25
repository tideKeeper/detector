#include <dlfcn.h>
#include <cstdio>
#include "lock_detect.h"
#include "output_control.h"

// 声明动态库导出的测试接口
extern "C" void StartNormalTest();
extern "C" void StartTrylockTest();
extern "C" void StartDeadlockTest();

int main() {
    // 1. 初始化输出控制
    tracker::OutputControl::Instance().Configure(OutputOption::OutputOption_ConsoleFile, "lock_detect_test.log");

    TRACKER_PRINT("===== 死锁检测模块测试（动态库模式）=====\n\n");

    // 2. 注册目标动态库，启动Hook
    // 关键：只Hook libtest_lock.so，不Hook主程序，彻底规避内部锁递归
    LockDetect::GetInstance().Register("libtest_lock.so");
    LockDetect::GetInstance().Start();

    // ---------- 测试1：正常加解锁 ----------
    TRACKER_PRINT("\n----- 测试1:正常加锁解锁 -----\n");
    StartNormalTest();
    LockDetect::GetInstance().Detect();
    TRACKER_PRINT("测试1 完成：正常场景无残留锁、无死锁\n\n");

    // ---------- 测试3：trylock 非阻塞 ----------
    TRACKER_PRINT("\n----- 测试3:trylock 非阻塞加锁 -----\n");
    StartTrylockTest();
    LockDetect::GetInstance().Detect();
    TRACKER_PRINT("测试3 完成:trylock 无残留锁、无误报\n\n");

    // ---------- 测试2：经典死锁场景（放最后） ----------
    TRACKER_PRINT("\n----- 测试2:双线程反向加锁死锁 -----\n");
    TRACKER_PRINT("即将触发死锁场景，检测模块会实时输出死锁报告\n");
    TRACKER_PRINT("触发后程序会因真实死锁阻塞，可手动 Ctrl+C 终止\n\n");
    StartDeadlockTest();

    TRACKER_PRINT("\n===== 全部测试完成 =====\n");
    return 0;
}
