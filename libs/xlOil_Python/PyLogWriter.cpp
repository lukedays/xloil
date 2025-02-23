#include "PyCore.h"
#include <xlOil/Log.h>
#include <xlOil/StringUtils.h>

using std::shared_ptr;
using std::wstring_view;
using std::vector;
using std::wstring;
using std::string;
namespace py = pybind11;

namespace xloil
{
  namespace Python
  {
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
          for (const auto& level_str : SPDLOG_LEVEL_NAMES)
          {
            if (strncmp(target.c_str(), level_str, target.length()) == 0)
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
          writeToLogImpl(message, toSpdLogLevel(level));
        }

        void writeToLogImpl(const char* message, spdlog::level::level_enum level)
        {
          if (!spdlog::default_logger_raw()->should_log(level))
            return;

          auto frame = PyEval_GetFrame();
          spdlog::source_loc source{ __FILE__, __LINE__, SPDLOG_FUNCTION };
          if (frame)
          {
            auto code = frame->f_code; // Guaranteed never null
            source.line = PyCode_Addr2Line(code, frame->f_lasti);
            source.filename = PyUnicode_AsUTF8(code->co_filename);
            source.funcname = PyUnicode_AsUTF8(code->co_name);
          }

          py::gil_scoped_release releaseGil;
          spdlog::default_logger_raw()->log(
            source,
            level,
            message);
        }

        void trace(const char* message) { writeToLogImpl(message, spdlog::level::trace); }
        void debug(const char* message) { writeToLogImpl(message, spdlog::level::debug); }
        void info(const char* message) { writeToLogImpl(message, spdlog::level::info); }
        void warn(const char* message) { writeToLogImpl(message, spdlog::level::warn); }
        void error(const char* message) { writeToLogImpl(message, spdlog::level::err); }

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

      static int theBinder = addBinder([](py::module& mod)
      {
        py::class_<LogWriter>(mod, 
          "_LogWriter", R"(
            Writes a log message to xlOil's log.  The level parameter can be a level constant 
            from the `logging` module or one of the strings *error*, *warn*, *info*, *debug* or *trace*.

            Only messages with a level higher than the xlOil log level which is initially set
            to the value in the xlOil settings will be output to the log file. Trace output
            can only be seen with a debug build of xlOil.
          )")
          .def(py::init<>(), R"(
            Do not construct this class - a singleton instance is created by xlOil.
          )")
          .def("__call__", 
            &LogWriter::writeToLog, 
            R"(
              Writes a message to the log at the optionally specifed level. The default 
              level is 'info'.
            )",
            py::arg("msg"),
            py::arg("level") = 20)
          .def("trace", &LogWriter::trace, 
            "Writes a log message at the 'trace' level",
            py::arg("msg"))
          .def("debug", &LogWriter::debug, 
            "Writes a log message at the 'debug' level", 
            py::arg("msg"))
          .def("info", &LogWriter::info, 
            "Writes a log message at the 'info' level", 
            py::arg("msg"))
          .def("warn", &LogWriter::warn, 
            "Writes a log message at the 'warn' level", 
            py::arg("msg"))
          .def("error", &LogWriter::error, 
            "Writes a log message at the 'error' level", 
            py::arg("msg"))
          .def_property("level", 
            &LogWriter::getLogLevel,
            &LogWriter::setLogLevel,
            R"(
              Returns or sets the current log level. The returned value will always be an 
              integer corresponding to levels in the `logging` module.  The level can be
              set to an integer or one of the strings *error*, *warn*, *info*, *debug* or *trace*.
            )");
      });
    }
  }
}