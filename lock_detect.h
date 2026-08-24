#pragma once

#include <memory>
#include <string>

// 前向声明impl类
class LockDetectImpl;

class LockDetect {
   public:
      static LockDetect& GetInstance() {
         static LockDetect instance;
         return instance;
      }

      void Register(const std::string& lib_name);
      void RegisterMain();
      void Start();
      void Detect();

      ~LockDetect();
      
   private:
      LockDetect();
      std::unique_ptr<LockDetectImpl> impl_;
};
