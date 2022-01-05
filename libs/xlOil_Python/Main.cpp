#include "Main.h"
#include "TypeConversion/Numpy.h"
#include "PyHelpers.h"
#include "PyEvents.h"
#include "TypeConversion/BasicTypes.h"
#include "PySource.h"
#include "PyFunctionRegister.h"
#include "EventLoop.h"

#include <xlOil/Register.h>
#include <xlOil/ExcelObj.h>
#include <xlOil/Log.h>
#include <xloil/Plugin.h>
#include <xloil/StringUtils.h>
#include <xlOil/Events.h>
#include <xlOil/ExcelThread.h>
#include <xlOil/ExcelUI.h>
#include <xlOilHelpers/Environment.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/embed.h>
#include <functional>
#include <cstdlib>
#include <filesystem>
#include <tomlplusplus/toml.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <signal.h>
#include <thread>
#include <CTPL/ctpl_stl.h>

namespace fs = std::filesystem;

using std::vector;
using std::wstring;
using std::string;
using std::function;
using std::shared_ptr;
using std::make_shared;

namespace py = pybind11;

// Some fun C signal handling to work-around the calling of abort when
// Py_Initialize fails. In Py >= 3.8, a two-step initialisation is 
// possible to avoid this problem.
namespace
{
#if PY_MAJOR_VERSION <= 3 && PY_MINOR_VERSION < 8
  jmp_buf longjumpBuffer;

  void signalHandler(int signum)
  {
    if (signum == SIGABRT)
    {
      // Reset signal handler
      signal(signum, SIG_DFL);
      // Never return - jump back to saved call site
      longjmp(longjumpBuffer, 1);
    }
  }
#else
  void checkReturnStatus(const PyStatus& status)
  {
    if (PyStatus_Exception(status))
      XLO_THROW("Cannot set PYTHONHOME: {}", status.err_msg ? status.err_msg : "Unknown error");
  }
#endif
}

namespace xloil
{
  namespace Python
  {
    namespace
    {
      std::map<wstring, PyAddin> theAddins;
      PyAddin* theCoreAddinContext;
      shared_ptr<const void> theWorkbookOpenHandler;

      PyAddin& findOrCreateAddin(AddinContext& ctx, bool newThread)
      {
        return theAddins.try_emplace(ctx.pathName(), ctx, newThread).first->second;
      }

      py::list pythonSysPath()
      {
        return PyBorrow<py::list>(PySys_GetObject("path"));
      }

      auto addinDir(AddinContext& ctx)
      {
        return fs::path(ctx.pathName()).remove_filename().wstring();
      }

      void startInterpreter(const std::wstring& setSysPath)
      {
        if (Py_IsInitialized())
          XLO_THROW(L"Python already initialised: Only one python plugin can be used");

        PyImport_AppendInittab(theInjectedModuleName, &buildInjectedModule);

        // The SetSysPath option can be useful when distributing an addin along
        // with all required python libs.

        if (!setSysPath.empty())
          Py_SetPath(setSysPath.c_str());

        XLO_DEBUG("Python interpreter starting");

        // Some fun C signal handling to work-around the calling of abort when
        // Py_Initialize fails. In Py >= 3.8, a two-step initialisation is 
        // possible to avoid this problem.
#if PY_VERSION_HEX < 0x03080000
        {
          // The first time setjmp executes the return value will be zero. 
          // If abort is called, execution will resume with setjmp returning
          // the value 1
          if (setjmp(longjumpBuffer) == 0)
          {
            signal(SIGABRT, &signalHandler);
            Py_InitializeEx(0);
            signal(SIGABRT, SIG_DFL);   // Reset signal handler
          }
          else
          {
            XLO_THROW("Python initialisation called abort. Check python lib paths make 'encodings' package available");
          }
        }
#else
        {
          PyConfig config;
          PyConfig_InitPythonConfig(&config);
          config.use_environment = 1; // It's the default, but just emphasising it!#
          config.parse_argv = 0;      // No point parsing cmd line args
          //checkReturnStatus(PyConfig_SetString(&config, &config.program_name, L"python.exe"));

          auto status = Py_InitializeFromConfig(&config);

          PyConfig_Clear(&config);
          checkReturnStatus(status);
        }
#endif

#if PY_VERSION_HEX < 0x03070000
        PyEval_InitThreads();
#endif

        // Release the GIL when we hand back control
        PyEval_SaveThread();
      }

      void exit()
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
      }
    }

    PyAddin::PyAddin(AddinContext& ctx, bool newThread)
      : context(ctx)
      , thread(newThread ? make_shared<EventLoop>() : theCoreAddin().thread)
    {}

    const std::wstring& PyAddin::pathName() const
    {
      return context.pathName();
    }

    PyAddin& findAddin(const wchar_t* xllPath)
    {
      return theAddins.find(xllPath)->second;
    }

    std::shared_ptr<EventLoop> getEventLoop()
    {
      auto id = std::this_thread::get_id();
      for (auto& [key, addin] : theAddins)
        if (addin.thread->thread().get_id() == id)
          return addin.thread;
      XLO_THROW("Internal: could not find addin associated with current thread");
    }

    PyAddin& theCoreAddin()
    {
      assert(theCoreAddinContext);
      return *theCoreAddinContext;
    }

    std::pair<std::shared_ptr<FileSource>, PyAddin*> findSource(const wchar_t* sourcePath)
    {
      auto [source, addin] = FileSource::findSource(sourcePath);
      return std::make_pair(source, addin ? &findAddin(addin->pathName().c_str()) : nullptr);
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
          assert(context);

          // On Load, we initialise the Python interpreter and import our 
          // pybind11 module
          linkPluginToCoreLogger(context, plugin);

          const auto setSysPath = utf8ToUtf16(plugin.settings["SetSysPath"].value_or(""));
          // At the moment numpy is required, so using this setting will give errors
          const auto numpySupport = plugin.settings["NumpySupport"].value_or(true);

          // Starts the python interpreter with our embedded module available
          startInterpreter(setSysPath);

          // startInterpreter releases the gil on completion
          py::gil_scoped_acquire getGil;

          auto pySysPath = pythonSysPath();
          pySysPath.append(py::wstr(addinDir(*context)));
          pySearchPath = py::str(pySysPath);

          // Since xloil imports importlib, it cannot be the first module imported by python
          // otherwise some bootstrap processes have not completed and xloil gets an incomplete
          // importlib module.
          // See https://stackoverflow.com/questions/39660934/error-when-using-importlib-util-to-check-for-library/39661116
          py::module::import("importlib.util");

          // https://bugs.python.org/issue37416
          py::module::import("threading");
          
          if (numpySupport)
          {
            XLO_DEBUG("Python importing numpy");
            if (!importNumpy())
              throw py::error_already_set();
          }
          importDatetime();

          XLO_DEBUG("Python importing xloil_core");
          py::module::import(theInjectedModuleName);

          // On Load, the core context is created with a new thread and event
          // loop. We must release gil before creating a PyAddin
          {
            py::gil_scoped_release releaseGil;
            auto& pyContext = findOrCreateAddin(*context, true);
            theCoreAddinContext = &pyContext;
          }

          XLO_DEBUG("Python importing xloil");
          py::module::import("xloil");
          py::module::import("xloil.importer").attr("_import_scan")(
            "xloil.excelfuncs", theCoreAddinContext->pathName());

          // Check the workbook module setting for loading local modules of the form 'Book1.py'.
          auto workbookModule = utf8ToUtf16(
            plugin.settings["WorkbookModule"].value_or("*.py"));

          if (!workbookModule.empty())
            theWorkbookOpenHandler = createWorkbookOpenHandler(workbookModule.c_str(), *theCoreAddinContext);

          return 0;
        }

        case PluginContext::Attach:
        {
          // Attach is called for each XLL which uses xlOil_Python.
          assert(context);
          auto& pyContext = findOrCreateAddin(
            *context, plugin.settings["SeparateThread"].value_or(false));

          // Set sys.path to include the attaching addin
          {
            py::gil_scoped_acquire gilAcquired;

            auto pySysPath = pythonSysPath();
            pySysPath.append(py::wstr(addinDir(*context)));
            pySearchPath = py::str(pySysPath);
          }

          pyContext.comBinder = plugin.settings["ComLib"].value_or("comtypes");

          // Load any modules requested in the settings file
          vector<string> modsToLoad;
          auto loadModules = plugin.settings["LoadModules"].as_array();
          if (loadModules)
            for (auto& m : *loadModules)
              modsToLoad.push_back(m.value_or<string>(""));

          // Given a pattern like *.py, try to find a python module of the form <xll-name>.py
          auto addinModule = plugin.settings["AddinModule"].value_or("*.py");
          auto star = addinModule.find('*');
          if (star != string::npos)
          {
            auto filename = utf8ToUtf16(addinModule);
            // Replace the star with our addin name (minus the extension)
            auto xllPath = fs::path(context->pathName()).replace_extension("");
            filename.replace(filename.find(L'*'), 1u, xllPath);

            std::error_code err;
            if (fs::exists(filename, err))
              modsToLoad.push_back(xllPath.stem().string());
          }

          {
            py::gil_scoped_acquire gilAcquired;
            pyContext.thread->callback("xloil.importer", "_import_scan", modsToLoad, pyContext.pathName());
          }

          return 0;
        }

        case PluginContext::Detach:
        {
          // On detach, remove the PyAddin object. Functions registered by
          // the addin will be remove by xlOil machinery
          theAddins.erase(context->pathName());
          return 0;
        }

        case PluginContext::Unload:
          theWorkbookOpenHandler.reset();
          theAddins.clear();
          theCoreAddinContext = nullptr;
          exit();
          return 0;
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
  }
}