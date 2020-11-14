#include "CppUnitTest.h"
#include <xloil/ExcelObjCache.h>
#include <chrono>
using namespace Microsoft::VisualStudio::CppUnitTestFramework;

using namespace xloil;
using std::wstring;
using std::make_unique;
using fmt::format;
using std::vector;
using std::unique_ptr;

namespace Tests
{
  TEST_CLASS(TestCache)
  {
  public:

    TEST_METHOD(ReverseLookupCacheTest)
    {
      ObjectCache<
        std::unique_ptr<int>, 
        CacheUniquifier<std::unique_ptr<int>>, 
        true> cache;
      const int N = 100;

      vector<ExcelObj> callers;
      vector<ExcelObj> keys(N);
      for (auto i = 0; i < N; ++i)
        callers.emplace_back(ExcelObj(format(L"Key_{0}", i)));

      for (auto i = 0; i < N; ++i)
        keys[i] = cache.add(make_unique<int>(i), CallerInfo(callers[i]));

      for (auto i = 0; i < N; ++i)
        cache.add(make_unique<int>(i), CallerInfo(callers[i]));

      for (auto i = 0; i < N; ++i)
      {
        auto* val = cache.fetch(keys[i].asPString().view());
        Assert::AreEqual<int>(i, **val);
        auto* key = cache.findKey(val);
        Assert::AreEqual(keys[i].toString(), *key);
      }

    }
    TEST_METHOD(CacheSpeedTest1)
    {
      auto& cache = ObjectCacheFactory<std::unique_ptr<int>>::cache();
      const int NumReps = 1;
      const int N = 100;

      vector<ExcelObj> callers;
      vector<ExcelObj> keys(N);
      for (auto i = 0; i < N; ++i)
        callers.emplace_back(ExcelObj(format(L"Key_{0}", i)));
      
      auto t1 = std::chrono::high_resolution_clock::now();

      for (auto i = 0; i < N; ++i)
        keys[i] = cache.add(make_unique<int>(i), CallerInfo(callers[i]));

      for (auto rep = 0; rep < NumReps; ++rep)
        for (auto i = 0; i < N; ++i)
          cache.add(make_unique<int>(i), CallerInfo(callers[i]));

      auto t2 = std::chrono::high_resolution_clock::now();

      for (auto rep = 0; rep < NumReps * 10; ++rep)
        for (auto i = 0; i < N; ++i)
        {
          auto* val = cache.fetch(keys[i].asPString().view());
#ifndef RUN_PERFORMANCE_TEST
          Assert::AreEqual<int>(i, **val);
#endif
        }

#ifndef RUN_PERFORMANCE_TEST
      auto t3 = std::chrono::high_resolution_clock::now();
      auto duration1 = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
      auto duration2 = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
      Logger::WriteMessage(format("CacheSpeedTest1 - Time 1: {0},   Time 2: {1}", duration1, duration2).c_str());
#endif
    }
  };
}