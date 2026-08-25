# Detector

面向 Linux 的动态库运行时检测工具，通过 PLT Hook 拦截目标模块中的内存分配与互斥锁操作，提供**内存泄漏检测**、**锁状态快照**和**实时死锁预警**。

检测器以 `detector.so` 形式提供，推荐通过 `dlopen` 加载，与业务主程序解耦，避免检测器内部锁与业务锁相互干扰。

## 功能

| 模块 | 能力 | 说明 |
|------|------|------|
| 内存检测 | 泄漏追踪 | Hook `malloc` / `free` / `calloc` / `realloc` 及 C++ `operator new/delete`，记录未释放分配及调用栈 |
| 锁检测 | 状态快照 | Hook `pthread_mutex_lock` / `unlock` / `trylock`，输出当前活跃锁、持有线程、等待关系 |
| 锁检测 | 死锁预警 | 在 `pthread_mutex_lock` **阻塞前**基于等待图做环路检测，发现潜在死锁立即打印报告 |

内部 `MemoryTracker` / `LockTracker` 使用互斥锁保护，**线程安全**；业务侧多线程并发分配或加锁时，记账会自动汇总到同一实例。

## 快速开始

### 环境要求

- Linux（x86_64，ELF）
- g++（C++17）
- `pthread`、`dl`

### 编译

```bash
# 编译检测器动态库
g++ -std=c++17 -Wall -Wextra -O0 -g \
    -fPIC -shared -DDETECTOR_BUILD_SHARED -fvisibility=hidden \
    -o detector.so \
    detector.cc lock_detect.cc memory_detect.cc output_control.cc plthook_elf64.cc \
    -ldl -lpthread

# 编译演示用业务库
g++ -std=c++17 -Wall -Wextra -O0 -g -fPIC -shared \
    -o libtest_demo.so test_demo_lib.cpp -lpthread

# 编译演示程序
g++ -std=c++17 -Wall -Wextra -O0 -g \
    -o test_detector_demo test_detector_demo.cc -ldl -lpthread
```

### 运行演示

```bash
./test_detector_demo              # 常规演示（内存 + 锁 + 多线程快照）
./test_detector_demo --deadlock   # 追加经典死锁场景（会阻塞，Ctrl+C 退出）
```

日志默认同时输出到控制台和当前目录下的 `detector_<timestamp>.log`。

## API 说明

头文件：`detector.h`。所有接口均为 C 链接，可从 C/C++ 调用。

### 配置枚举

```c
// 检测模式（可按位组合）
enum DetectorOption {
    DetectorOption_Memory     = 1,  // 内存泄漏检测
    DetectorOption_Lock       = 2,  // 死锁 / 锁状态检测
    DetectorOption_MemoryLock = 3,  // 同时启用
};

// 输出方式（可按位组合）
enum OutputOption {
    OutputOption_Console     = 1,  // 控制台
    OutputOption_File        = 2,  // 文件
    OutputOption_ConsoleFile = 3,  // 控制台 + 文件
};
```

### 调用顺序

```
Detector_Init → Detector_Register / Detector_RegisterMain → Detector_Start → Detector_Detect
```

| 函数 | 说明 |
|------|------|
| `Detector_Init(work_dir, detect_option, output_option)` | 初始化检测器。`work_dir` 为日志输出目录；须最先调用 |
| `Detector_Register(lib_name)` | 注册待检测的动态库路径，可多次调用；须在 `Start` 之前 |
| `Detector_RegisterMain()` | 注册主程序自身；须在 `Start` 之前 |
| `Detector_Start()` | 安装 PLT Hook，开始追踪；须在 `Register` 完成之后调用 |
| `Detector_Detect()` | 输出当前内存泄漏报告和/或锁状态快照；可在程序任意检查点调用 |

### 最小集成示例

```c
#include "detector.h"

int main() {
    Detector_Init(".", DetectorOption_MemoryLock, OutputOption_ConsoleFile);

    Detector_Register("./libyour_app.so");  // 注册业务动态库
    Detector_RegisterMain();                // 同时检测主程序

    Detector_Start();   // 此后分配 / 加锁才会被追踪

    // ... 运行业务逻辑 ...

    Detector_Detect();  // 检查点：输出泄漏与锁状态
    return 0;
}
```

### 通过 dlopen 加载（推荐）

将检测器编译为独立 `.so`，在运行时加载，可避免与主程序符号冲突：

```c
void* handle = dlopen("./detector.so", RTLD_LAZY | RTLD_GLOBAL);
auto Init = (void(*)(const char*, DetectorOption, OutputOption))
            dlsym(handle, "Detector_Init");
auto Start = (void(*)()) dlsym(handle, "Detector_Start");
// ... 其余 API 同理 ...

Init(".", DetectorOption_MemoryLock, OutputOption_ConsoleFile);
// Register → Start → Detect
```

完整示例见 `test_detector_demo.cc`。

## 使用建议

1. **注册顺序**：先 `Register` 所有目标库和主程序，再调用 `Start`；Hook 只作用于已注册模块的 PLT 表。
2. **检查时机**：`Detect()` 是快照接口。若需查看「正在持有的锁」，应在线程仍持锁时调用，而不是 `join` 之后。
3. **死锁检测**：预警在 `pthread_mutex_lock` 进入阻塞前自动触发，无需调用 `Detect()`。
4. **多线程**：检测器本身不需要多线程运行；业务多线程时 hook 自动生效。如需长驻进程周期性采样，可由单独监控线程调用 `Detect()`。
5. **调用栈解析**：泄漏报告依赖 `addr2line`，请确保目标二进制以 `-g` 编译。

## 项目结构

```
detector.h / detector.cc       # 对外 C API 与模块调度
memory_detect.h / .cc          # 内存分配追踪
lock_detect.h / .cc            # 锁状态与死锁检测
output_control.h / .cc         # 日志输出控制
plthook.h / plthook_elf64.cc   # ELF PLT Hook 实现
test_detector_demo.cc          # 完整 API 演示（dlopen 模式）
test_demo_lib.cpp              # 演示用业务动态库
test_lock_detect.cc            # 锁检测单元测试
test_plthook.cc                # PLT Hook 测试
```

## 限制

- 仅支持 Linux ELF x86_64；通过 PLT 替换实现 Hook，目标函数须出现在对应模块的 PLT 中。
- 主程序若未将 `pthread_mutex_*` 等符号放入 PLT，可能无法 Hook（启动时会打印 WARNING）。
- `Detect()` 输出未做并发串行化，建议从单一线程调用以避免日志交错。

## License

见仓库授权说明（如未单独声明，请联系维护者）。
