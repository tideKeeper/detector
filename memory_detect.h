#pragma once

#include <memory>
#include <string>

// 前向声明impl类
class MemoryDetectImpl;
// 内存检测类
class MemoryDetect {
public:
  // 单例模式, 静态局部变量保证线程安全
  static MemoryDetect &GetInstance() {
    static MemoryDetect instance;
    return instance;
  };

  // 将指定的动态库添加到内存检测范围中
  void Register(const std::string &lib_name);

  // 将主进程添加到内存检测范围中
  void RegisterMain();

  // 启动, 并且追踪内存分配和释放
  void Start();

  // 检测内存泄漏
  void Detect();

  ~MemoryDetect();

private:
  // 私有构造函数
  MemoryDetect();

  std::unique_ptr<MemoryDetectImpl> impl_;
};
