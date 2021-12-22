#include "PyCore.h"
#include "PyHelpers.h"
#include "PyEvents.h"
#include "EventLoop.h"
#include <TypeConversion/BasicTypes.h>
#include <xlOil/ExcelApp.h>
#include <xloil/Log.h>
#include <xloil/Caller.h>
#include <xloil/AppObjects.h>
#include <xlOil/StringUtils.h>
#include <xlOil/ExcelUI.h>
#include <map>

using std::shared_ptr;
using std::vector;
using std::wstring;
using std::string;
namespace py = pybind11;
using std::make_pair;

namespace xloil 
{
  namespace Python 
  {
    using BinderFunc = std::function<void(pybind11::module&)>;

    PyTypeObject* cellErrorType;
    PyObject*     comBusyException;
    PyObject*     cannotConvertException;
    shared_ptr<const IPyToExcel> theCustomReturnConverter = nullptr;

    namespace
    {
      auto cleanupGlobals = Event_PyBye().bind([] {
        theCustomReturnConverter.reset();
      });
      void initialiseCore(py::module& mod);
    }

    class BinderRegistry
    {
    public:
      static BinderRegistry& get() {
        static BinderRegistry instance;
        return instance;
      }

      auto add(BinderFunc f, size_t priority)
      {
        return theFunctions.insert(make_pair(priority, f));
      }

      void bindAll(py::module& mod)
      {
        std::for_each(theFunctions.rbegin(), theFunctions.rend(),
          [&mod](auto f) { f.second(mod); });
      }
    private:
      BinderRegistry() {}
      std::multimap<size_t, BinderFunc> theFunctions;
    };

    PyObject* buildInjectedModule()
    {
      auto mod = py::module(theInjectedModuleName);
      initialiseCore(mod);
      BinderRegistry::get().bindAll(mod);
      return mod.release().ptr();
    }

    int addBinder(std::function<void(pybind11::module&)> binder)
    {
      BinderRegistry::get().add(binder, 1);
      return 0;
    }

    namespace
    {
      // The numerical values of the python log levels align nicely with spdlog
      // so we can translate with a factor of 10.
      // https://docs.python.org/3/library/logging.html#levels

      class LogWriter
      {
      public:

        /// <summary>
        /// Allows intial match like 'warn' for 'warning'
        /// </summary>
        /// <param name="target"></param>
        /// <returns></returns>
        spdlog::level::level_enum levelFromStr(const std::string& target)
        {
          using namespace spdlog::level;
          int iLevel = 0;
          for (const auto& level_str : level_string_views)
          {
            if (strncmp(target.c_str(), level_str.data(), target.length()) == 0)
              return static_cast<level_enum>(iLevel);
            iLevel++;
          }
          return off;
        }

        spdlog::level::level_enum toSpdLogLevel(const py::object& level)
        {
          if (PyLong_Check(level.ptr()))
          {
            return spdlog::level::level_enum(
              std::min(PyLong_AsUnsignedLong(level.ptr()) / 10, 6ul));
          }
          return levelFromStr(toLower((string)py::str(level)));
        }

        void writeToLog(const char* message, const py::object& level)
        {
          auto frame = PyEval_GetFrame();
          spdlog::source_loc source{ __FILE__, __LINE__, SPDLOG_FUNCTION };
          if (frame)
          {
            auto code = frame->f_code; // Guaranteed never null
            source.line = PyCode_Addr2Line(code, frame->f_lasti);
            source.filename = PyUnicode_AsUTF8(code->co_filename);
            source.funcname = PyUnicode_AsUTF8(code->co_name);
          }
          spdlog::default_logger_raw()->log(
            source,
            toSpdLogLevel(level),
            message);
        }

        unsigned getLogLevel()
        {
          auto level = spdlog::default_logger()->level();
          return level * 10;
        }

        void setLogLevel(const py::object& level)
        {
          spdlog::default_logger()->set_level(toSpdLogLevel(level));
        }
      };

      void runLater(const py::object& callable, int nRetries, int retryPause, int delay)
      {
        runExcelThread([callable]()
          {
            py::gil_scoped_acquire getGil;
            try
            {
              callable();
            }
            catch (py::error_already_set& err)
            {
              if (err.matches(comBusyException))
                throw ComBusyException();
              throw;
            }
          },
          ExcelRunQueue::WINDOW | ExcelRunQueue::COM_API,
          nRetries,
          retryPause,
          delay);
      }

      void setReturnConverter(const shared_ptr<const IPyToExcel>& conv)
      {
        theCustomReturnConverter = conv;
      }

      struct CannotConvert {};

      struct PyStatusBar
      {
        std::unique_ptr<StatusBar> _bar;

        PyStatusBar(size_t timeout)
          : _bar(new StatusBar(timeout))
        {}
        void msg(const std::wstring& msg, size_t timeout)
        {
          _bar->msg(msg, timeout);
        }
        void exit(py::args)
        {
          _bar.reset();
        }
      };

      void initialiseCore(pybind11::module& mod)
      {
        // Bind the two base classes for python converters
        py::class_<IPyFromExcel, shared_ptr<IPyFromExcel>>(mod, "IPyFromExcel")
          .def("__call__",
            [](const IPyFromExcel& /*self*/, const py::object& /*arg*/)
        {
          XLO_THROW("Internal IPyFromExcel converters cannot be called from python");
        });

        py::class_<IPyToExcel, shared_ptr<IPyToExcel>>(mod, "IPyToExcel");

        mod.def("set_return_converter", setReturnConverter);

        mod.def("in_wizard", &inFunctionWizard);

        py::class_<LogWriter>(mod, "LogWriter")
          .def(py::init<>())
          .def("__call__", &LogWriter::writeToLog, py::arg("msg"), py::arg("level") = 20)
          .def_property("level", &LogWriter::getLogLevel, &LogWriter::setLogLevel);

        // TODO: consider rename to app_run ?
        mod.def("excel_run",
          &runLater,
          py::arg("func"),
          py::arg("num_retries") = 10,
          py::arg("retry_delay") = 500,
          py::arg("wait_time") = 0);

        py::class_<App::ExcelInternals>(mod, "ExcelState")
          .def_readonly("version", &App::ExcelInternals::version)
          .def_readonly("hinstance", &App::ExcelInternals::hInstance)
          .def_readonly("hwnd", &App::ExcelInternals::hWnd)
          .def_readonly("main_thread_id", &App::ExcelInternals::mainThreadId);

        // TODO: rename to app_state? (also in xloil.py)
        mod.def("excel_state", App::internals);

        py::class_<CallerInfo>(mod, "Caller")
          .def(py::init<>())
          .def_property_readonly("sheet",
            [](const CallerInfo& self)
            {
              const auto name = self.sheetName();
              return name.empty() ? py::none() : py::wstr(wstring(name));
            })
          .def_property_readonly("workbook",
            [](const CallerInfo& self)
            {
              const auto name = self.workbook();
              return name.empty() ? py::none() : py::wstr(wstring(name));
            })
          .def("address",
            [](const CallerInfo& self, bool x)
            {
              return self.writeAddress(x ? CallerInfo::A1 : CallerInfo::RC);
            }, py::arg("a1style") = false);


        py::class_<PyStatusBar>(mod, "StatusBar")
          .def(py::init<size_t>(), py::arg("timeout") = 0)
          .def("__enter__", [](py::object self) { return self; })
          .def("__exit__", &PyStatusBar::exit)
          .def("msg", &PyStatusBar::msg, py::arg("msg"), py::arg("timeout") = 0);

        comBusyException = py::register_exception<ComBusyException>(mod, "ComBusyError").ptr();

        cannotConvertException = py::exception<CannotConvert>(mod, "CannotConvert").ptr();

        {
          // Bind CellError type to xloil::CellError enum
          auto eType = py::enum_<CellError>(mod, "CellError");
          for (auto e : theCellErrors)
            eType.value(utf16ToUtf8(enumAsWCString(e)).c_str(), e);

          cellErrorType = (PyTypeObject*)eType.ptr();
        }

        mod.def("get_event_loop", [](const wchar_t* addin) { findAddin(addin).thread->loop(); });
      }
    }
} }