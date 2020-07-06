#include "AsyncFunctions.h"
#include "FunctionRegister.h"
#include "InjectedModule.h"
#include "BasicTypes.h"
#include "PyHelpers.h"
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
    constexpr const char* THREAD_CONTEXT_TAG = "xloil_thread_context";


    struct EventLoopController
    {
      std::atomic<bool> _stop = true;
      PyObject* _runLoopFunction = nullptr;
      std::future<void> _loopRunner;
      ctpl::thread_pool _thread;
      std::shared_ptr<const void> _shutdownHandler;

      EventLoopController()
        : _thread(1)
      {
        // We create a hanging reference in gil_scoped_aquire to prevent it 
        // destroying the python thread state. The thread state contains thread 
        // locals used by asyncio to find the event loop for that thread and avoid
        // creating a new one.
        _thread.push([](int)
        {
          py::gil_scoped_acquire acquire;
          acquire.inc_ref();
        });

        _shutdownHandler = std::static_pointer_cast<const void>(
          Event_PyBye().bind([self = this]
        {
          self->shutdown();
        }));
      }

      void start()
      {
        

        py::gil_scoped_acquire gilAcquired;

        // This should always be run on the main thread, also the 
        // GIL gives us a mutex.
        if (!_runLoopFunction)
        {
          const auto xloilModule = py::module::import("xloil");
          _runLoopFunction = xloilModule.attr("_pump_message_loop").ptr();
        }

        _stop = false;

        _loopRunner = _thread.push(
          [=](int)
        {
          py::gil_scoped_acquire gilAcquired;
          try
          {
            PyBorrow<py::function>(_runLoopFunction).call(this);
          }
          catch (const std::exception& e)
          {
            XLO_ERROR("Error running asyncio loop: {0}", e.what());
          }
        }
        );
      }
      /// <summary>
      /// Executes the function on the python asyncio thread. The thread is normally
      /// running the asyncio event loop.  This stops and restarts the loop to allow
      /// the function to run.
      /// </summary>
      template<typename F>
      void runInterrupt(F && f)
      {
        auto task = _thread.push(f);
        stop();
        start();
      }

      bool stopped()
      {
        return _stop;
      }

    private:
      void stop()
      {
        _stop = true;
        // Must wait for the loop to stop, otherwise we may switch the stop
        // flag back without it ever being check
        if (_loopRunner.valid())
          _loopRunner.wait();
      }

      void shutdown()
      {
        stop();
        // Resolve the hanging reference in gil_scoped_aquire and destroy
        // the python thread state
        _thread.push([](int)
        {
          py::gil_scoped_acquire acquire;
          acquire.dec_ref();
        }).wait();
        _thread.stop();
      }
    };

    auto& getEventLoop()
    {
      static std::unique_ptr<EventLoopController> p(new EventLoopController());
      return *p;
    }


    struct AsyncReturn : public AsyncHelper
    {
      AsyncReturn(
        const ExcelObj& asyncHandle,
        const shared_ptr<IPyToExcel>& returnConverter)
        : AsyncHelper(asyncHandle)
        , _returnConverter(returnConverter)
      {}

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

      void cancel() override
      {
        if (_task.ptr())
        {
          py::gil_scoped_acquire gilAcquired;
          _task.attr("cancel").call();
          _task.release();
        }
      }

    private:
      shared_ptr<IPyToExcel> _returnConverter;
      py::object _task;
    };

    void pythonAsyncCallback(
      PyFuncInfo* info,
      const ExcelObj* asyncHandle,
      const ExcelObj** xlArgs) noexcept
    {
      try
      {
        PyObject *argsP, *kwargsP;
        AsyncReturn* asyncReturn;

        {
          py::gil_scoped_acquire gilAcquired;

          PyErr_Clear();

          // I think it's better to process the arguments to python here rather than 
          // copying the ExcelObj's and converting on the async thread (since CPython
          // is single threaded anyway)
          auto[args, kwargs] = info->convertArgs(xlArgs);
          if (kwargs.is_none())
            kwargs = py::dict();

          // Raw ptr, but we take ownership below
          asyncReturn = new AsyncReturn(
            *asyncHandle,
            info->returnConverter);

          // Kwargs will be dec-refed after the asyncReturn is used
          kwargs[THREAD_CONTEXT_TAG] = py::cast(asyncReturn,
            py::return_value_policy::take_ownership);

          // Need to drop pybind links before lambda capture in otherwise the lambda's 
          // dtor is called at some random time after losing the GIL and it crashes.
          argsP = args.release().ptr();
          kwargsP = kwargs.release().ptr();
        }

        auto functor = [info, argsP, kwargsP, asyncReturn](int threadId) mutable
        {
          py::gil_scoped_acquire gilAcquired;
          {
            try
            {
              // This will return through the asyncReturn object
              info->invoke(argsP, kwargsP);
            }
            catch (const std::exception& e)
            {
              asyncReturn->result(ExcelObj(e.what()));
            }
            Py_XDECREF(argsP);
            Py_XDECREF(kwargsP);
          }
        };
        getEventLoop().runInterrupt(functor);
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
        const shared_ptr<IPyToExcel>& returnConverter,
        const wchar_t* caller)
        : _notify(notify)
        , _returnConverter(returnConverter)
        , _caller(caller)
      {}
      ~RtdReturn()
      {
        // TODO: maybe use a raw PyObject to avoid needing this?
        if (_hasTask)
        {
          py::gil_scoped_acquire gilAcquired;
          _task.release();
        }
      }
      void set_task(const py::object& task)
      {
        py::gil_scoped_acquire gilAcquired;
        _task = task;
        _hasTask = true;
      }
      void set_result(const py::object& value)
      {
        py::gil_scoped_acquire gilAcquired;
        ExcelObj result = _returnConverter
          ? (*_returnConverter)(*value.ptr())
          : FromPyObj()(value.ptr(), false);
        // If nil, conversion wasn't possible, so use the cache
        if (result.isType(ExcelType::Nil))
          result = pyCacheAdd(value, _caller);
        _notify.publish(std::move(result));
      }

      void cancel()
      {
        if (_hasTask)
        {
          py::gil_scoped_acquire gilAcquired;
          if (!_hasTask)
            return;
          _hasTask = false;
          _task.attr("cancel").call();
          _task.release();
        }
      }
      bool done()
      {
        if (!_hasTask) 
          return true;

        py::gil_scoped_acquire gilAcquired;
        return _hasTask 
          ? py::cast<bool>(_task.attr("done").call())
          : true;
      }
      void wait()
      {
        if (!_hasTask)
          return;

        py::gil_scoped_acquire gilAcquired;
        if (_hasTask)
          _task.attr("wait").call();
      }

    private:
      IRtdPublish& _notify;
      shared_ptr<IPyToExcel> _returnConverter;
      py::object _task;
      std::atomic<bool> _hasTask = false;
      const wchar_t* _caller;
    };

    /// <summary>
    /// Holder for python target function and its arguments.
    /// Able to compare arguments with another AsyncTask
    /// </summary>
    struct AsyncTask : public IRtdAsyncTask
    {
      PyFuncInfo* _info;
      PyObject *_args, *_kwargs;
      RtdReturn* _returnObj = nullptr;
      wstring _caller;

      /// <summary>
      /// Steals references to PyObjects
      /// </summary>
      AsyncTask(PyFuncInfo* info, PyObject* args, PyObject* kwargs)
        : _info(info)
        , _args(args)
        , _kwargs(kwargs)
        , _caller(CallerInfo().writeAddress(false))
      {}

      virtual ~AsyncTask()
      {
        py::gil_scoped_acquire gilAcquired;
        Py_XDECREF(_args);
        Py_XDECREF(_kwargs);
        delete _returnObj;
      }

      void start(IRtdPublish& publish) override
      {
        _returnObj = new RtdReturn(publish, _info->returnConverter, _caller.c_str());

        getEventLoop().runInterrupt(
          [=](int /*threadId*/)
          {
            py::gil_scoped_acquire gilAcquired;

            PyErr_Clear();

            auto kwargs = PyBorrow<py::object>(_kwargs);
            kwargs[THREAD_CONTEXT_TAG] = _returnObj;

            _info->invoke(_args, kwargs.ptr());
          }
        );
      }
      bool done() override
      {
        return !_returnObj || _returnObj->done();
      }
      void wait() override
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
        const auto* that = dynamic_cast<const AsyncTask*>(&that_);
        if (!that)
          return false;

        py::gil_scoped_acquire gilAcquired;

        auto args = PyBorrow<py::tuple>(_args);
        auto kwargs = PyBorrow<py::dict>(_kwargs);
        auto that_args = PyBorrow<py::tuple>(that->_args);
        auto that_kwargs = PyBorrow<py::dict>(that->_kwargs);

        if (args.size() != that_args.size()
          || kwargs.size() != that_kwargs.size())
          return false;

        for (auto i = args.begin(), j = that_args.begin();
          i != args.end();
          ++i, ++j)
        {
          if (!i->equal(*j))
            return false;
        }
        for (auto i = kwargs.begin(); i != kwargs.end(); ++i)
        {
          if (!i->first.equal(py::str(THREAD_CONTEXT_TAG))
            && !i->second.equal(that_kwargs[i->first]))
            return false;
        }
        return true;
      }
    };

    ExcelObj* pythonRtdCallback(
      PyFuncInfo* info,
      const ExcelObj** xlArgs) noexcept
    {
      try
      {
        // TODO: consider argument capture and equality check under c++
        PyObject *argsP, *kwargsP;
        {
          py::gil_scoped_acquire gilAcquired;

          auto[args, kwargs] = info->convertArgs(xlArgs);
          if (kwargs.is_none())
            kwargs = py::dict();

          // Add this here so that dict sizes for running and newly 
          // created tasks match
          kwargs[THREAD_CONTEXT_TAG] = py::none();

          // Need to drop pybind links before capturing in lambda otherwise the destructor
          // is called at some random time after losing the GIL and it crashes.
          argsP = args.release().ptr();
          kwargsP = kwargs.release().ptr();
        }

        auto value = rtdAsync(
          std::make_shared<AsyncTask>(info, argsP, kwargsP));
        return returnValue(value ? *value : CellError::NA);
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
          .def("set_task", &AsyncReturn::set_task);

        py::class_<RtdReturn>(mod, "RtdReturn")
          .def("set_result", &RtdReturn::set_result)
          .def("set_task", &RtdReturn::set_task);

        py::class_<EventLoopController>(mod, "EventLoopController")
          .def("stopped", &EventLoopController::stopped);

        // This is a module level string so async function can find the
        // control object we add to their keyword args.
        mod.add_object("ASYNC_CONTEXT_TAG", py::str(THREAD_CONTEXT_TAG));
      });
    }
  }
}