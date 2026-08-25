/**
 * @file detector.cc
 * @brief 检测器模块实现文件
 *
 * 该文件实现了检测器的核心功能，包括初始化、启动、检测和注册等操作。
 * 检测器支持内存泄漏检测和死锁检测两种模式，可以通过配置选项进行控制。
 */

#include "detector.h"

#include <time.h>
#include <string>

#include "lock_detect.h"
#include "memory_detect.h"
#include "output_control.h"

/**
 * @brief 生成输出文件路径
 * @param work_dir 工作目录
 * @return 完整的输出文件路径
 *
 * 根据工作目录和当前时间戳生成唯一的输出文件名
 */
std::string GetFilePath(std::string work_dir) {
    std::string output_file_name = work_dir + "/detector_" + std::to_string(time(nullptr)) + ".log";
    return output_file_name;
}

extern "C" {
// 默认启用内存和锁检测
static DetectorOption detector_option = DetectorOption_MemoryLock;

// 初始化检测器
// work_dir 工作目录
// detect_option 检测选项
// output_option 输出选项
// 配置工作模式和输出方法
void Detector_Init(const char* work_dir, DetectorOption detect_option, OutputOption output_option) {
    detector_option = detect_option;
    std::string output_file_name = GetFilePath(work_dir);
    tracker::OutputControl::Instance().Configure(output_option, output_file_name);
}

// 执行检测
void Detector_Detect(void) {
    if (detector_option & DetectorOption_Memory) {
        MemoryDetect::GetInstance().Detect();
    }
    if (detector_option & DetectorOption_Lock) {
        LockDetect::GetInstance().Detect();
    }
}

// 启动检测器
void Detector_Start(void) {
    if (detector_option & DetectorOption_Memory) {
        MemoryDetect::GetInstance().Start();
    }
    if (detector_option & DetectorOption_Lock) {
        LockDetect::GetInstance().Start();
    }
}

// 注册指定库
void Detector_Register(const char* lib_name) {
    if (lib_name == nullptr || lib_name[0] == '\0') {
        return;
    }
    if (detector_option & DetectorOption_Memory) {
        MemoryDetect::GetInstance().Register(lib_name);
    }
    if (detector_option & DetectorOption_Lock) {
        LockDetect::GetInstance().Register(lib_name);
    }
}

// 注册主程序
void Detector_RegisterMain(void) {
    if (detector_option & DetectorOption_Memory) {
        MemoryDetect::GetInstance().RegisterMain();
    }
    if (detector_option & DetectorOption_Lock) {
        LockDetect::GetInstance().RegisterMain();
    }
}
}