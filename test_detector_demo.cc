/**
 * @file test_detector_demo.cc
 * @brief 通过 dlopen(detector.so) 演示完整 Detector API 流程
 *
 * 覆盖：Init / Register / RegisterMain / Start / Detect
 *       内存泄漏检测、锁状态检测、多线程快照、实时死锁预警（可选）
 *
 * 用法：
 *   ./test_detector_demo              # 常规演示（不含死锁阻塞场景）
 *   ./test_detector_demo --deadlock   # 追加死锁场景（会阻塞，Ctrl+C 退出）
 */

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "detector.h"

static constexpr const char* kDetectorSo = "./detector.so";
static constexpr const char* kDemoLibSo = "./libtest_demo.so";

using DemoFn = void (*)();

struct DemoLibApi {
    void* handle = nullptr;
    DemoFn NormalAlloc = nullptr;
    DemoFn MemoryLeak = nullptr;
    DemoFn NormalLock = nullptr;
    DemoFn Trylock = nullptr;
    DemoFn Deadlock = nullptr;
    DemoFn MtMemory = nullptr;
    DemoFn MtLockHoldStart = nullptr;
    DemoFn MtLockHoldStop = nullptr;
    DemoFn MtLockWaitStart = nullptr;
    DemoFn MtLockWaitStop = nullptr;
    DemoFn MtBusyStart = nullptr;
    DemoFn MtBusyStop = nullptr;
};

using DetectorInitFn = void (*)(const char*, DetectorOption, OutputOption);
using DetectorStartFn = void (*)();
using DetectorDetectFn = void (*)();
using DetectorRegisterFn = void (*)(const char*);
using DetectorRegisterMainFn = void (*)();

struct DetectorApi {
    void* handle = nullptr;
    DetectorInitFn Init = nullptr;
    DetectorStartFn Start = nullptr;
    DetectorDetectFn Detect = nullptr;
    DetectorRegisterFn Register = nullptr;
    DetectorRegisterMainFn RegisterMain = nullptr;
};

static bool LoadDemoLib(DemoLibApi& demo) {
    demo.handle = dlopen(kDemoLibSo, RTLD_LAZY | RTLD_GLOBAL);
    if (!demo.handle) {
        fprintf(stderr, "dlopen %s 失败: %s\n", kDemoLibSo, dlerror());
        return false;
    }

    demo.NormalAlloc = (DemoFn)dlsym(demo.handle, "DemoSoNormalAlloc");
    demo.MemoryLeak = (DemoFn)dlsym(demo.handle, "DemoSoMemoryLeak");
    demo.NormalLock = (DemoFn)dlsym(demo.handle, "DemoSoNormalLock");
    demo.Trylock = (DemoFn)dlsym(demo.handle, "DemoSoTrylock");
    demo.Deadlock = (DemoFn)dlsym(demo.handle, "DemoSoDeadlock");
    demo.MtMemory = (DemoFn)dlsym(demo.handle, "DemoSoMtMemory");
    demo.MtLockHoldStart = (DemoFn)dlsym(demo.handle, "DemoSoMtLockHoldStart");
    demo.MtLockHoldStop = (DemoFn)dlsym(demo.handle, "DemoSoMtLockHoldStop");
    demo.MtLockWaitStart = (DemoFn)dlsym(demo.handle, "DemoSoMtLockWaitStart");
    demo.MtLockWaitStop = (DemoFn)dlsym(demo.handle, "DemoSoMtLockWaitStop");
    demo.MtBusyStart = (DemoFn)dlsym(demo.handle, "DemoSoMtBusyStart");
    demo.MtBusyStop = (DemoFn)dlsym(demo.handle, "DemoSoMtBusyStop");

    if (!demo.NormalAlloc || !demo.MemoryLeak || !demo.NormalLock || !demo.Trylock || !demo.Deadlock ||
        !demo.MtMemory || !demo.MtLockHoldStart || !demo.MtLockHoldStop || !demo.MtLockWaitStart ||
        !demo.MtLockWaitStop || !demo.MtBusyStart || !demo.MtBusyStop) {
        fprintf(stderr, "dlsym libtest_demo.so 接口失败: %s\n", dlerror());
        dlclose(demo.handle);
        demo.handle = nullptr;
        return false;
    }
    return true;
}

static void UnloadDemoLib(DemoLibApi& demo) {
    if (demo.handle) {
        dlclose(demo.handle);
        demo.handle = nullptr;
    }
}

static bool LoadDetectorApi(DetectorApi& api) {
    api.handle = dlopen(kDetectorSo, RTLD_LAZY | RTLD_GLOBAL);
    if (!api.handle) {
        fprintf(stderr, "dlopen %s 失败: %s\n", kDetectorSo, dlerror());
        return false;
    }

    api.Init = (DetectorInitFn)dlsym(api.handle, "Detector_Init");
    api.Start = (DetectorStartFn)dlsym(api.handle, "Detector_Start");
    api.Detect = (DetectorDetectFn)dlsym(api.handle, "Detector_Detect");
    api.Register = (DetectorRegisterFn)dlsym(api.handle, "Detector_Register");
    api.RegisterMain = (DetectorRegisterMainFn)dlsym(api.handle, "Detector_RegisterMain");

    if (!api.Init || !api.Start || !api.Detect || !api.Register || !api.RegisterMain) {
        fprintf(stderr, "dlsym Detector API 失败: %s\n", dlerror());
        dlclose(api.handle);
        api.handle = nullptr;
        return false;
    }
    return true;
}

static void UnloadDetectorApi(DetectorApi& api) {
    if (api.handle) {
        dlclose(api.handle);
        api.handle = nullptr;
    }
}

static void PrintBanner(const char* title) {
    printf("\n========================================\n");
    printf("  %s\n", title);
    printf("========================================\n");
}

// 主程序内故意泄漏，用于验证 RegisterMain
static void DemoMainMemoryLeak() {
    printf("[Main Memory] 主程序故意泄漏 512 字节 (malloc) + 64 字节 (new)\n");
    (void)malloc(512);
    (void)new int[16];
}

struct MonitorArg {
    DetectorDetectFn detect = nullptr;
    std::atomic<int>* stop = nullptr;
};

// 独立监控线程周期性调用 Detect：检测模块本身不需要线程池，
// 但 Detect() 可以从任意线程调用，适合主线程被占满时做快照。
static void* MonitorDetectThread(void* arg) {
    auto* a = static_cast<MonitorArg*>(arg);
    int n = 0;
    while (!a->stop->load()) {
        usleep(80 * 1000);
        if (a->stop->load()) {
            break;
        }
        printf("\n[Monitor] 监控线程 %lu 第 %d 次 Detect()\n", (unsigned long)pthread_self(), ++n);
        fflush(stdout);
        a->detect();
    }
    return nullptr;
}

int main(int argc, char* argv[]) {
    const bool run_deadlock = (argc > 1 && strcmp(argv[1], "--deadlock") == 0);

    PrintBanner("Detector 全功能演示（dlopen 模式）");

    // ----------------------------------------------------------------
    // 1. 加载 detector.so，获取 API 函数指针
    // ----------------------------------------------------------------
    DetectorApi det;
    if (!LoadDetectorApi(det)) {
        return 1;
    }
    printf("[Setup] 已加载 detector.so，API 函数指针就绪\n");

    // ----------------------------------------------------------------
    // 2. 初始化：检测模式 + 输出方式
    // ----------------------------------------------------------------
    // 日志写到当前目录（项目文件夹），Windows 下可直接看到 detector_*.log
    det.Init(".", DetectorOption_MemoryLock, OutputOption_ConsoleFile);
    printf("[Setup] Detector_Init 完成（内存+锁，控制台+文件输出）\n");
    printf("[Setup] 日志文件: 当前目录下 detector_<timestamp>.log\n");
    printf("[Setup] 请在 WSL/Linux 终端运行本程序，例如: ./test_detector_demo\n");
    fflush(stdout);

    // ----------------------------------------------------------------
    // 3. 加载并注册业务动态库
    // ----------------------------------------------------------------
    DemoLibApi demo;
    if (!LoadDemoLib(demo)) {
        UnloadDetectorApi(det);
        return 1;
    }
    det.Register(kDemoLibSo);
    printf("[Setup] 已注册 %s\n", kDemoLibSo);

    // ----------------------------------------------------------------
    // 4. 注册主程序（detector 在独立 .so 中，内部锁不会递归）
    // ----------------------------------------------------------------
    det.RegisterMain();
    printf("[Setup] 已注册主程序 (RegisterMain)\n");

    // ----------------------------------------------------------------
    // 5. 启动 hook（此后分配/加锁才会被追踪）
    // ----------------------------------------------------------------
    det.Start();
    printf("[Setup] Detector_Start 完成，hook 已激活\n");

    // ----------------------------------------------------------------
    // 阶段 1：.so 内正常内存操作（应无泄漏）
    // ----------------------------------------------------------------
    PrintBanner("阶段 1：.so 正常内存分配/释放");
    demo.NormalAlloc();
    det.Detect();

    // ----------------------------------------------------------------
    // 阶段 1b：多线程并发内存（join 后再 Detect，应只多 1 处泄漏）
    // ----------------------------------------------------------------
    PrintBanner("阶段 1b：多线程并发内存（4 线程，仅 1 处泄漏）");
    demo.MtMemory();
    det.Detect();

    // ----------------------------------------------------------------
    // 阶段 1c：两线程同时持有不同锁时做快照（join 之前 Detect）
    // ----------------------------------------------------------------
    PrintBanner("阶段 1c：多线程持锁快照（Detect 时锁仍被持有）");
    demo.MtLockHoldStart();
    det.Detect();
    demo.MtLockHoldStop();

    // ----------------------------------------------------------------
    // 阶段 1d：一线程持锁、另一线程阻塞等待时做快照
    // ----------------------------------------------------------------
    PrintBanner("阶段 1d：多线程等待快照（持有者 + 等待者）");
    demo.MtLockWaitStart();
    det.Detect();
    demo.MtLockWaitStop();

    // ----------------------------------------------------------------
    // 阶段 1e：工作线程忙碌时，由独立监控线程周期性 Detect
    // ----------------------------------------------------------------
    PrintBanner("阶段 1e：监控线程周期性 Detect（工作线程仍在跑）");
    {
        std::atomic<int> stop{0};
        MonitorArg marg{det.Detect, &stop};
        pthread_t monitor;
        demo.MtBusyStart();
        pthread_create(&monitor, nullptr, MonitorDetectThread, &marg);
        usleep(220 * 1000);
        stop.store(1);
        pthread_join(monitor, nullptr);
        demo.MtBusyStop();
    }

    // ----------------------------------------------------------------
    // 阶段 2：.so 内故意泄漏
    // ----------------------------------------------------------------
    PrintBanner("阶段 2：.so 内存泄漏");
    demo.MemoryLeak();
    det.Detect();

    // ----------------------------------------------------------------
    // 阶段 3：主程序内故意泄漏（验证 RegisterMain）
    // ----------------------------------------------------------------
    PrintBanner("阶段 3：主程序内存泄漏 (RegisterMain)");
    DemoMainMemoryLeak();
    det.Detect();

    // ----------------------------------------------------------------
    // 阶段 4：.so 正常加解锁（应无残留锁）
    // ----------------------------------------------------------------
    PrintBanner("阶段 4：.so 正常加解锁");
    demo.NormalLock();
    det.Detect();

    // ----------------------------------------------------------------
    // 阶段 5：.so trylock（应无残留锁）
    // ----------------------------------------------------------------
    PrintBanner("阶段 5：.so trylock 非阻塞加锁");
    demo.Trylock();
    det.Detect();

    // ----------------------------------------------------------------
    // 阶段 6（可选）：死锁 — hook 会在加锁前实时输出预警，随后阻塞
    // ----------------------------------------------------------------
    if (run_deadlock) {
        PrintBanner("阶段 6：.so 经典死锁（会阻塞）");
        printf("检测器会在形成环路等待时实时打印死锁报告\n");
        printf("程序随后会因真实死锁卡住，请 Ctrl+C 终止\n\n");
        demo.Deadlock();
    } else {
        printf("\n提示: 运行 ./test_detector_demo --deadlock 可演示实时死锁检测\n");
    }

    PrintBanner("演示结束");
    UnloadDemoLib(demo);
    UnloadDetectorApi(det);
    return 0;
}
