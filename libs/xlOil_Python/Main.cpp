#include "Numpy.h"
#include "PyHelpers.h"
#include "BasicTypes.h"
#include "ReadSource.h"
#include "FunctionRegister.h"

#include <xlOil/Register.h>
#include <xlOil/ExcelObj.h>
#include <xlOil/Log.h>
#include <xloil/Plugin.h>
#include <xloil/StringUtils.h>
#include <xlOil/Events.h>
#include <xlOil/ApiCall.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/embed.h>
#include <functional>
#include <cstdlib>
#include <filesystem>
#include <tomlplusplus/toml.hpp>
#include <boost/preprocessor/stringize.hpp>

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

    XLOIL_DEFINE_EVENT(Event_PyBye);

    int exit()
    {
      try
      {
        Event_PyBye().fire();
      }
      catch (const std::exception& e)
      {
        XLO_ERROR("PyBye: {0}", e.what());
      }
      try
      {
        PyGILState_Ensure();
        py::finalize_interpreter();
      }
      catch (...)
      {
      }
      return 0;
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
            XLO_THROW(L"Python already initialised: Only one python plugin can be used");

          linkLogger(context, plugin);

          PyImport_AppendInittab(theInjectedModuleName, &buildInjectedModule);

          XLO_DEBUG("Python interpreter starting");
          py::initialize_interpreter();
          PyEval_InitThreads(); // Not needed after Py 3.7

          // At the moment numpy is required, so using this setting will give errors
          bool numpySupport = plugin.settings["NumpySupport"].value_or(true);
          if (numpySupport)
          {
            XLO_DEBUG("Python importing numpy");
            if (!importNumpy())
              throw py::error_already_set();
          }
          importDatetime();
          
          theCoreContext = context;

          XLO_DEBUG("Python importing xloil_core");
          py::module::import(theInjectedModuleName);

          // Check the workbook module setting for loading local module 
          // of the form 'Book1.py'.
          auto workbookModule = utf8ToUtf16(
            plugin.settings["WorkbookModule"].value_or("*.py"));
          if (!workbookModule.empty())
            createWorkbookOpenHandler(workbookModule.c_str());
          
          // Release the GIL when we hand back control
          PyEval_SaveThread(); 

          return 0;
        }

        case PluginContext::Attach:
        {
          // On attach, we set our sys.path and load the modules requested
          // in the settings file
          py::gil_scoped_acquire gilAcquired;

          theCurrentContext = context;

          
          const auto ourDir = fs::path(context->pathName()).remove_filename();
          const auto pyPath = PyBorrow<py::list>(PySys_GetObject("path"));
          const auto pyOurDir = py::wstr(ourDir.c_str());

          // TODO: could lead to multiple copies if an addin is unloaded and reloaded
          pyPath.append(pyOurDir);
          pySearchPath = py::str(pyPath);

          // Since xloil imports importlib, it cannot be the first module imported by python
          // otherwise some bootstrap processes have not completed and xloil gets an incomplete
          // importlib module.
          // See https://stackoverflow.com/questions/39660934/error-when-using-importlib-util-to-check-for-library/39661116
          py::module::import("importlib.util");

          XLO_DEBUG("Python importing xloil");
          const auto xloilModule = py::module::import("xloil");
          scanModule(py::str("xloil.excelfuncs"));

          vector<wstring> modsToLoad;

          auto loadModules = plugin.settings["LoadModules"].as_array();
          if (loadModules)
            for (auto& m : *loadModules)
              modsToLoad.push_back(utf8ToUtf16(m.value_or<string>("")));

          // Given a pattern like *.py, try to find a python module of the form <xll-name>.py
          auto addinModule = plugin.settings["AddinModule"].value_or("*.py");
          auto star = addinModule.find('*');
          if (star != string::npos)
          {
            auto filename = utf8ToUtf16(addinModule);
            // Replace the star with our addin name (minus the extension)
            auto xllPath = fs::path(context->pathName());
            xllPath.replace_extension("");
            filename.replace(filename.find(L'*'), 1u, xllPath);

            std::error_code err;
            if (fs::exists(filename, err))
              modsToLoad.push_back(xllPath.stem());
          }

          // TODO: initialise entire plugin in background!
          for (auto& m : modsToLoad)
          {
            XLO_DEBUG(L"Python importing {0}", m);
            scanModule(py::wstr(m));
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
        return PySteal<py::object>(PyLong_FromVoidPtr(&excelApp()));
      }
      static int theBinder = addBinder([](py::module& mod)
      {
        mod.def("application", &getExcelApp);
      });
    }
  }
}