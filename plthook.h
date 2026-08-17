#pragma once

#include <memory>
#include <string>

class PLTHook {
public:
  static constexpr int SUCCESS = 0;
  // 未找到文件
  static constexpr int FILE_NOT_FOUND = -1;
  static constexpr int INVALID_ARGUMENT = -2;
  // 未找到函数
  static constexpr int FUNCTION_NOT_FOUND = -3;
  //
  static constexpr int INTERNAL_ERROR = -4;
  // 已经访问到文末
  static constexpr int EOF_REACHED = -5;

  /*创建实例
  针对指定库的plt钩子
  file_name 是目标的库名称, 如果为空, 则指向主程序
  返回的是执行hook实例的唯一指针, 失败返回 nullptr
  */
  static std::unique_ptr<PLTHook> Create(const char *file_name);

  /*
  替换PLT表中的函数
  const char* func_name 是要替换的函数名
  new_func 是新的函数指针
  old_func 存储原函数的地址, 这样可以保障原功能被执行
  */
  int ReplaceFunction(const char *func_name, void *new_func,
                      void **old_func_out = nullptr);

  /*
  获取最近的错误信息
  操作失败时获得错误信息
  */
  static const std::string &GetLastError();

  ~PLTHook();

private:
  struct Impl;
  std::unique_ptr<Impl> pimpl_;

  PLTHook();

  PLTHook(const PLTHook &) = delete;
  PLTHook &operator=(const PLTHook &) = delete;
};