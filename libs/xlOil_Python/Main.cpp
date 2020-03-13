
#include "xloil/Register.h"
#include "ExcelObj.h"
#include "xloil/Log.h"
#include "xloil/Interface.h"
#include "xloil/Loader.h"
#include "xloil/Settings.h"
#include "xloil/Utils.h"
#include "xloil/Events.h"
#include "Numpy.h"
#include "PyHelpers.h"
#include "BasicTypes.h"
#include "FunctionRegister.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <functional>
#include <cstdlib>
#include <filesystem>
#include <toml11/toml.hpp>

namespace fs = std::filesystem;

using std::shared_ptr;
using std::vector;
using std::wstring;
using std::string;
using std::function;
namespace py = pybind11;

namespace xloil
{
  namespace Python
  {
    extern Core* theCore = nullptr;

    Event<void(void), VoidCollector>& Event_PyBye() { static std::remove_reference<decltype(Event_PyBye())>::type e; return e; }

    XLO_PLUGIN_EXIT()
    {
      try
      {
        Event_PyBye().fire();
        theCore->deregisterAll();
        PyGILState_Ensure();
        Py_Finalize();
      }
      catch (...)
      {
      }
      return TRUE;
    }

    extern "C" __declspec(dllexport) int xloil_buildId()
    {
      return 0;
    }

    XLO_PLUGIN_INIT(Core& interface)
    {
      // Used to give more helpful errors as most problems at this stage are path-related
      string pySearchPath;
      try
      {
        theCore = &interface;
         
        spdlog::set_default_logger(theCore->getLogger());

        auto settingsPtr = interface.settings();
        auto settings = settingsPtr ? *settingsPtr : toml::value();

        if (Py_IsInitialized())
          XLO_THROW(L"Only one python plugin can be used: Python already initialised when loading {0}", interface.pluginName());

        auto modsToLoad = toml::find_or<vector<string>>(settings, "LoadModules", vector<string>());

        // This impacts the search path
        auto xllPath = const_cast<wchar_t*>(Core::theCorePath());
        // Py_SetProgramName(xllPath); // supposed to add dirname(xllPath) to search path. 
        PyImport_AppendInittab(theInjectedModuleName, &buildInjectedModule);

        Py_Initialize();      // Initialise python interpreter
        PyEval_InitThreads(); // Not needed after Py 3.7

        bool numpySupport = toml::find_or<bool>(settings, "NumpySupport", true);
        if (numpySupport)
          importNumpy();

        importDatetime();
        createCache();

        auto ourDir = fs::path(xllPath).remove_filename().string();
        auto pyPath = PyBorrow<py::list>(PySys_GetObject("path"));
        auto pyOurDir = py::str(ourDir.c_str());
        pyPath.append(pyOurDir);
        pySearchPath = py::str(pyPath);

        py::module::import(theInjectedModuleName);

        for (auto& m : modsToLoad)
          scanModule(py::str(m));

        auto workbookModule = utf8ToUtf16(toml::find_or<string>(settings, "WorkbookModule", "*.py"));
        if (!workbookModule.empty())
        {
          if (workbookModule.find(L"*") == wstring::npos)
            XLO_ERROR("WorkbookModule should be of the form '*foo.py' where '*'"
              "will be replaced by the full workbook path with file extension removed");
          else
          {
            workbookModule.replace(workbookModule.find('*'), 1, wstring(L"{0}\\{1}"));
            static auto wbOpenHandler = Event_WorkbookOpen().bind(
              [workbookModule](const wchar_t* wbPath, const wchar_t* wbName)
              {
                // workbookModule is already a printf format string, so we just need to add
                // the workbook name with the extension removed.
                auto modName = fmt::format(workbookModule, wbPath, wstring(wbName, wcsrchr(wbName, L'.')));
                if (!fs::exists(modName))
                  return;
                try
                {
                  FunctionRegistry::get().addModule(modName);
                  scanModule(py::wstr(modName));
                }
                catch (const std::exception& e)
                {
                  XLO_WARN(L"Failed to load module {0}: {1}", modName, utf8ToUtf16(e.what()));
                }
              }
            );
          }
        }
      }
      catch (const std::exception& e)
      {
        XLO_ERROR("xloil_python init failed: {0}. sys.path={1}", e.what(), pySearchPath);
        return -1;
      }
      catch (...)
      {
        return -1;
      }
      
      PyEval_SaveThread();
      return 0;
    }

    namespace
    {
      py::object getExcelApp()
      {
        return PySteal<py::object>(PyLong_FromVoidPtr(&theCore->theExcelApp()));
      }
      static int theBinder = addBinder([](py::module& mod)
      {
        mod.def("application", &getExcelApp);
      });
    }
  }
}