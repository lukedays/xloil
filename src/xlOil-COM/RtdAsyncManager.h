#pragma once
#include <xloil/RtdServer.h>
#include <xloil/StringUtils.h>
#include <shared_mutex>

namespace xloil
{
  namespace COM
  {
    struct CellTasks;

    class RtdAsyncManager
    {
    public:
      static RtdAsyncManager& instance();

      /// <summary>
      /// Given an RtdAsync Task, returns a value, if one is has
      /// already been published, or starts the task and subscribes.
      /// This triggers a callback from Excel when a value is available
      /// </summary>
      /// <param name="task"></param>
      /// <returns></returns>
      std::shared_ptr<const ExcelObj>
        getValue(
          const std::shared_ptr<IRtdAsyncTask>& task);

      /// <summary>
      /// Destroys all running Rtd Async tasks.  Used on teardown
      /// </summary>
      void clear();
    
      using CellAddress = std::pair<unsigned, unsigned>;
      using CellTaskMap = std::unordered_map<
        CellAddress,
        std::shared_ptr<CellTasks>,
        pair_hash<unsigned, unsigned>>;

    private:
      std::shared_ptr<IRtdServer> _rtd;
      CellTaskMap _tasksPerCell;
      mutable std::shared_mutex _mutex;

      RtdAsyncManager();
    };
  }
}