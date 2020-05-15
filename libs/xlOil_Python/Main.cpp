#include <xlOil/Register.h>
#include <xlOil/ExcelObj.h>
#include <xlOil/Log.h>
#include <xlOil/Interface.h>
#include <xloilHelpers/StringUtils.h>
#include <xlOil/Events.h>
#include "Numpy.h"
#include "PyHelpers.h"
#include "BasicTypes.h"
#include "File.h"
#include "FunctionRegister.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/embed.h>
#include <functional>
#include <cstdlib>
#include <filesystem>
#include <tomlplusplus/toml.hpp>

namespace fs = std::filesystem;

using std::vector;
using std::wstring;
using std::string;
using std::function;
namespace py = pybind11;

namespace xloil
{
  namespace Python
  {
    extern AddinContext* theCoreContext = nullptr;
    extern AddinContext* theCurrentContext = nullptr;

    Event::Event<void(void), detail::VoidCollector>& Event_PyBye()
    { 
      static std::remove_reference<decltype(Event_PyBye())>::type e("PyBye"); return e;
    }

    int exit()
    {
      try
      {
        Event_PyBye().fire();
        PyGILState_Ensure();
        py::finalize_interpreter();
      }
      catch (...)
      {
      }
      return 0;
    }

    void createWorkbookModuleHandler(const wstring& starredPattern)
    {
      if (starredPattern.find(L"*") == wstring::npos)
        XLO_WARN("WorkbookModule should be of the form '*foo.py' where '*'"
          "will be replaced by the full workbook path with file extension removed");
      else
      {
        // Turn the starred pattern into a fmt string for easier substitution later
        auto workbookModule = starredPattern;
        workbookModule.replace(workbookModule.find(L'*'), 1, wstring(L"{0}\\{1}"));

        static auto wbOpenHandler = Event::WorkbookOpen().bind(
          [workbookModule](const wchar_t* wbPath, const wchar_t* wbName)
        {
          // Subtitute in to find target module name, removing extension
          auto modName = fmt::format(workbookModule, 
            wbPath, 
            wstring(wbName, wcsrchr(wbName, L'.')));

          if (!fs::exists(modName))
            return;
          try
          {
            // First add the module, if the scan fails it will still be on the
            // file change watchlist
            FunctionRegistry::addModule(theCoreContext, modName, wbName);
            scanModule(py::wstr(modName), wbName);
          }
          catch (const std::exception& e)
          {
            XLO_WARN(L"Failed to load module {0}: {1}", modName, utf8ToUtf16(e.what()));
          }
        }
        );
      }
    }

    extern "C" __declspec(dllexport) int xloil_buildId()
    {
      return 0;
    }

    XLO_PLUGIN_INIT(AddinContext* context, const PluginContext& plugin)
    {
      // Used to give more helpful errors as most problems at this stage are path-related
      string pySearchPath;
      try
      {
        switch (plugin.action)
        {
        case PluginContext::Load:
        {
          // On Load, we initialise the Python interpreter and import our
          // pybind11 injected module

          if (Py_IsInitialized())
            XLO_THROW(L"Only one python plugin can be used: "
              "Python already initialised when loading {0}", L"xloil_Python");

          linkLogger(context, plugin);

          PyImport_AppendInittab(theInjectedModuleName, &buildInjectedModule);

          py::initialize_interpreter();
          PyEval_InitThreads(); // Not needed after Py 3.7

          // At the moment numpy is required, so using this setting will give errors
          bool numpySupport = (*plugin.settings)["NumpySupport"].value_or(true);
          if (numpySupport)
            importNumpy();

          importDatetime();
          
          theCoreContext = context;
          
          py::module::import(theInjectedModuleName);

          // Check the workbook module setting for loading local module 
          // of the form 'Book1.py'.
          auto workbookModule = utf8ToUtf16(
            (*plugin.settings)["WorkbookModule"].value_or("*.py"));
          if (!workbookModule.empty())
            createWorkbookModuleHandler(workbookModule);

          // If we don't call this, the next attempt to get the GIL will
          // deadlock
          PyEval_SaveThread(); 

          return 0;
        }

        case PluginContext::Attach:
        {
          // On attach, we set our sys.path and load the modules requested
          // in the settings file
          py::gil_scoped_acquire gilAcquired;

          theCurrentContext = context;

          auto modsToLoad = (*plugin.settings)["LoadModules"].as_array();
          const auto ourDir = fs::path(context->pathName()).remove_filename();
          const auto pyPath = PyBorrow<py::list>(PySys_GetObject("path"));
          const auto pyOurDir = py::wstr(ourDir.c_str());

          // TODO: could lead to multiple copies if an addin is unloaded and reloaded
          pyPath.append(pyOurDir);
          pySearchPath = py::str(pyPath);

          if (modsToLoad)
            for (auto& m : *modsToLoad)
              scanModule(py::str(m.value_or<string>("Bad LoadModules element")));

          auto addinModule = (*plugin.settings)["AddinModule"].value_or("*.py");
          
          auto star = addinModule.find('*');
          if (star != string::npos)
          {
            auto filename = utf8ToUtf16(addinModule);
            filename.replace(star, 1u, 
              fs::path(context->pathName()).stem());
            filename.erase(filename.find(L".py"));
            scanModule(py::wstr(filename));
          }
        }

        case PluginContext::Detach:
        {
          // On detach we currently do nothing. Functions registered by
          // the exiting addin will be remove by xlOil machinery
          return 0;
        }

        case PluginContext::Unload:
          return exit();
        }
      }
      catch (const std::exception& e)
      {
        XLO_ERROR("xloil_python init failed: {0}. sys.path={1}", e.what(), pySearchPath);
      }
      catch (...)
      {
      }
      return -1;
    }

    namespace
    {
      py::object getExcelApp()
      {
        return PySteal<py::object>(PyLong_FromVoidPtr(&Core::theExcelApp()));
      }
      static int theBinder = addBinder([](py::module& mod)
      {
        mod.def("application", &getExcelApp);
      });
    }
  }
}