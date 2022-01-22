#include "AsyncFunctions.h"
#include "PyFunctionRegister.h"
#include "PyCore.h"
#include "TypeConversion/BasicTypes.h"
#include "PyHelpers.h"
#include "PyEvents.h"
#include "EventLoop.h"
#include <xloil/ExcelObj.h>
#include <xloil/Async.h>
#include <xloil/RtdServer.h>
#include <xloil/StaticRegister.h>
#include <xloil/Caller.h>
#include <CTPL/ctpl_stl.h>
#include <vector>

using std::shared_ptr;
using std::vector;
using std::pair;
using std::wstring;
using std::string;
using std::make_shared;
using std::make_pair;
namespace py = pybind11;

namespace xloil
{
  namespace Python
  {
    auto& asyncEventLoop()
    {
      return *theCoreAddin().thread;
    }

    struct AsyncReturn : public AsyncHelper
    {
      AsyncReturn(
        const ExcelObj& asyncHandle,
        const shared_ptr<const IPyToExcel>& returnConverter,
        CallerInfo&& caller)
        : AsyncHelper(asyncHandle)
        , _returnConverter(returnConverter)
        , _caller(std::move(caller))
      {
      }

      ~AsyncReturn()
      {
        try
        {
          cancel();
        }
        catch (...) {}
      }

      void set_task(const py::object& task)
      {
        _task = task;
      }

      void set_result(const py::object& value)
      {
        py::gil_scoped_acquire gil;
        static ExcelObj obj = _returnConverter
          ? (*_returnConverter)(*value.ptr())
          : FromPyObj()(value.ptr());
        result(obj);
      }
      void set_done()
      {}
      
      void cancel() override
      {
        if (_task.ptr())
        {
          py::gil_scoped_acquire gilAcquired;
          if (py::hasattr(_task, "cancel"))
            asyncEventLoop().callback(_task.attr("cancel"));
          _task.release();
        }
      }

      const CallerInfo& caller() const noexcept
      {
        return _caller;
      }

    private:
      shared_ptr<const IPyToExcel> _returnConverter;
      py::object _task;
      CallerInfo _caller;
    };

    void pythonAsyncCallback(
      const PyFuncInfo* info,
      const ExcelObj** xlArgs) noexcept
    {
      const ExcelObj* asyncHandle = xlArgs[0];

      try
      {
        py::gil_scoped_acquire gilAcquired;

        PyErr_Clear();

        // I think it's better to process the arguments to python here rather than 
        // copying the ExcelObj's and converting on the async thread (since CPython
        // is single threaded anyway)
        PyCallArgs<> pyArgs;

        // Raw ptr, but we take ownership in the next line
        auto* asyncReturn = new AsyncReturn(
          *asyncHandle,
          info->getReturnConverter(),
          CallerInfo());

        pyArgs.push_back(py::cast(asyncReturn,
          py::return_value_policy::take_ownership).release().ptr());

        py::object kwargs;
        info->convertArgs([&](auto i) { return *xlArgs[1 + i]; },
          pyArgs,
          kwargs);

        pyArgs.call(info->func().ptr(), kwargs.ptr());
      }
      catch (const py::error_already_set& e)
      {
        raiseUserException(e);
        asyncReturn(*asyncHandle, ExcelObj(e.what()));
      }
      catch (const std::exception& e)
      {
        XLO_WARN(e.what());
        asyncReturn(*asyncHandle, ExcelObj(e.what()));
      }
      catch (...)
      {
        XLO_WARN("Async unknown error");
        asyncReturn(*asyncHandle, ExcelObj(CellError::Value));
      }
    }

    struct RtdReturn
    {
      RtdReturn(
        IRtdPublish& notify,
        const shared_ptr<const IPyToExcel>& returnConverter,
        const CallerInfo& caller)
        : _notify(notify)
        , _returnConverter(returnConverter)
        , _caller(caller)
      {
      }
      ~RtdReturn()
      {
        if (!_running && !_task.ptr())
          return;

        py::gil_scoped_acquire gilAcquired;
        _running = false;
        _task = py::object();
      }
      void set_task(const py::object& task)
      {
        py::gil_scoped_acquire gilAcquired;
        _task = task;
        _running = true;
      }
      void set_result(const py::object& value) const
      {
        if (!_running)
          return;
        py::gil_scoped_acquire gilAcquired;

        // Convert result to ExcelObj
        ExcelObj result = _returnConverter
          ? (*_returnConverter)(*value.ptr())
          : FromPyObj<false>()(value.ptr());

        // If nil, conversion wasn't possible, so use the cache
        if (result.isType(ExcelType::Nil))
          result = pyCacheAdd(value, _caller.writeInternalAddress().c_str());

        _notify.publish(std::move(result));
      }
      void set_done()
      {
        if (!_running)
          return;
        py::gil_scoped_acquire gilAcquired;
        _running = false;
        _task = py::object();
      }
      void cancel()
      {
        if (!_running)
          return;
        py::gil_scoped_acquire gilAcquired;
        _running = false;
        asyncEventLoop().callback(_task.attr("cancel"));
      }
      bool done() noexcept
      {
        return !_running;
      }
      void wait() noexcept
      {
        // asyncio.Future has no 'wait'
      }
      const CallerInfo& caller() const noexcept
      {
        return _caller;
      }

    private:
      IRtdPublish& _notify;
      shared_ptr<const IPyToExcel> _returnConverter;
      py::object _task;
      std::atomic<bool> _running = true;
      const CallerInfo& _caller;
    };

    /// <summary>
    /// Holder for python target function and its arguments.
    /// Able to compare arguments with another AsyncTask
    /// </summary>
    struct RtdAsyncTask : public IRtdAsyncTask
    {
      const PyFuncInfo& _info;
      vector<ExcelObj> _xlArgs;
      shared_ptr<RtdReturn> _returnObj;
      CallerInfo _caller;

      RtdAsyncTask(const PyFuncInfo& info, const ExcelObj** xlArgs)
        : _info(info)
      {
        const auto nArgs = info.info()->numArgs();
        _xlArgs.reserve(nArgs);
        for (auto i = 0u; i < nArgs; ++i)
          _xlArgs.emplace_back(ExcelObj(*xlArgs[i]));
      }

      virtual ~RtdAsyncTask()
      {
        _returnObj.reset();
      }

      void start(IRtdPublish& publish) override
      {
        _returnObj.reset(new RtdReturn(publish, _info.getReturnConverter(), _caller));
        py::gil_scoped_acquire gilAcquired;
        
        PyErr_Clear(); // TODO: required?
        py::object kwargs;
        PyCallArgs<> pyArgs;

        pyArgs.push_back(py::cast(_returnObj).release().ptr());

        _info.convertArgs([&](auto i) -> const ExcelObj& { return _xlArgs[i]; }, 
          pyArgs,
          kwargs);
        
        pyArgs.call(_info.func().ptr(), kwargs.ptr());
      }
      bool done() noexcept override
      {
        return _returnObj ? _returnObj->done() : false;
      }
      void wait() noexcept override
      {
        if (_returnObj)
          _returnObj->wait();
      }
      void cancel() override
      {
        if (_returnObj)
          _returnObj->cancel();
      }
      bool operator==(const IRtdAsyncTask& that_) const override
      {
        const auto* that = dynamic_cast<const RtdAsyncTask*>(&that_);
        if (!that)
          return false;

        if (_xlArgs.size() != that->_xlArgs.size())
          return false;

        for (auto i = _xlArgs.begin(), j = that->_xlArgs.begin();
          i != _xlArgs.end();
          ++i, ++j)
        {
          if (!(*i == *j))
            return false;
        }

        return true;
      }
    };

    ExcelObj* pythonRtdCallback(
      const PyFuncInfo* info,
      const ExcelObj** xlArgs) noexcept
    {
      try
      {
        auto value = rtdAsync(
          std::make_shared<RtdAsyncTask>(*info, xlArgs));

        return returnValue(value ? *value : CellError::NA);
      }
      catch (const py::error_already_set& e)
      {
        raiseUserException(e);
        return returnValue(e.what());
      }
      catch (const std::exception& e)
      {
        return returnValue(e.what());
      }
      catch (...)
      {
        return returnValue(CellError::Null);
      }
    }
    namespace
    {
      static int theBinder = addBinder([](py::module& mod)
      {
        py::class_<AsyncReturn>(mod, "AsyncReturn")
          .def("set_result", &AsyncReturn::set_result)
          .def("set_done", &AsyncReturn::set_done)
          .def("set_task", &AsyncReturn::set_task)
          .def_property_readonly("caller", &AsyncReturn::caller)
          .def_property_readonly("loop", [](py::object x) { return asyncEventLoop().loop(); });

        py::class_<RtdReturn, shared_ptr<RtdReturn>>(mod, "RtdReturn")
          .def("set_result", &RtdReturn::set_result)
          .def("set_done", &RtdReturn::set_done)
          .def("set_task", &RtdReturn::set_task)
          .def_property_readonly("caller", &RtdReturn::caller)
          .def_property_readonly("loop", [](py::object x) { return asyncEventLoop().loop(); });

        mod.def("get_async_loop", []() { return asyncEventLoop().loop(); });
      });

      // Uncomment this for debugging in case weird things happen with the GIL not releasing
      //static auto gilCheck = Event::AfterCalculate().bind([]() { XLO_INFO("PyGIL State: {}", PyGILState_Check());  });
    }
  }
}