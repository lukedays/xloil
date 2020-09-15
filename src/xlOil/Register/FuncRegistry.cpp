#include "FuncRegistry.h"
#include <xlOil/Register.h>
#include <xlOil/ExcelCall.h>
#include <xlOil/Events.h>
#include <xlOilHelpers/PEHelper.h>
#include <xlOil/ExcelObj.h>
#include <xlOil/StaticRegister.h>
#include <xlOil/Log.h>
#include <xlOil/StringUtils.h>
#include <xlOil/State.h>
#include <xlOil/Loaders/EntryPoint.h>
#include <xlOil/Async.h>
#include <xlOil/Preprocessor.h>
#include <xlOilHelpers/Thunker.h>
#include <unordered_set>
#include <codecvt>
#include <future>
#include <map>
#include <filesystem>
namespace fs = std::filesystem;

using std::vector;
using std::shared_ptr;
using std::unique_ptr;
using std::string;
using std::wstring;
using std::unordered_set;
using std::map;
using std::make_shared;
using namespace msxll;

namespace xloil
{
  XLOIL_EXPORT FuncInfo::~FuncInfo()
  {
  }

  XLOIL_EXPORT bool FuncInfo::operator==(const FuncInfo & that) const
  {
    return name == that.name && help == that.help && category == that.category
      && options == that.options && std::equal(args.begin(), args.end(), that.args.begin(), that.args.end());
  }
}

namespace xloil
{
  constexpr char* XLOIL_STUB_NAME_STR = XLO_STR(XLOIL_STUB_NAME);

  // With Win32 function names are decorated. It no longer seemed
  // like a good idea with x64.
  std::string decorateCFunction(const char* name, const size_t numPtrArgs)
  {
#ifdef _WIN64
    (void)numPtrArgs;
    return string(name);
#else
    return fmt::format("_{0}@{1}", name, sizeof(void*) * numPtrArgs);
#endif // _WIN64
  }

  class FunctionRegistry
  {
  public:
    static FunctionRegistry& get() {
      static FunctionRegistry instance;
      return instance;
    }

    // TODO: We can allocate wipthin our DLL's address space by using
    // NtAllocateVirtualMemory or VirtualAlloc with MEM_TOP_DOWN
    // Currently this gives space for about 1500 thunks
    static constexpr auto theCaveSize = 16384 * 8u;
    static char theCodeCave[theCaveSize];

    /// <summary>
    /// The next available spot in our code cave
    /// </summary>
    static char* theCodePtr;

    ExcelObj theCoreDllName;

    template <class TCallback>
    auto callBuildThunk(
      TCallback callback,
      const void* contextData,
      const size_t numArgs)
    {
      // TODO: cache thunks with same number of args and callback?

      const size_t codeBufferSize = sizeof(theCodeCave) + theCodeCave - theCodePtr;
      size_t codeBytesWritten;
#ifdef _WIN64
      auto* thunk = buildThunkLite(callback, contextData, numArgs,
        theCodePtr, codeBufferSize, codeBytesWritten);
#else
      auto* thunk = buildThunk(callback, contextData, numArgs,
        theCodePtr, codeBufferSize, codeBytesWritten);
#endif
      XLO_ASSERT(thunk == (void*)theCodePtr);
      theCodePtr += codeBytesWritten;
      return std::make_pair(thunk, codeBytesWritten);
    }

    /// <summary>
    /// Locates a suitable entry point in our DLL and hooks the specifed thunk to it
    /// </summary>
    /// <param name="info"></param>
    /// <param name="thunk"></param>
    /// <returns>The name of the entry point selected</returns>
    auto hookEntryPoint(const FuncInfo& info, const void* thunk)
    {
      // Hook the thunk by modifying the export address table
      XLO_DEBUG(L"Hooking thunk for {0}", info.name);
      
      theExportTable->hook(theFirstStub, (void*)thunk);

      const auto entryPoint = decorateCFunction(XLOIL_STUB_NAME_STR, 0);

#ifdef _DEBUG
      // Check the thunk is hooked to Windows' satisfaction
      void* procNew = GetProcAddress((HMODULE)State::coreModuleHandle(), 
        entryPoint.c_str());
      XLO_ASSERT(procNew == thunk);
#endif

      return entryPoint;
    }

    static int registerWithExcel(
      shared_ptr<const FuncInfo> info, 
      const char* entryPoint, 
      const ExcelObj& moduleName)
    {
      auto numArgs = info->args.size();
      int opts = info->options;

      // Build the argument type descriptor 
      string argTypes;

      // Set function option prefixes
      if (opts & FuncInfo::ASYNC)
        argTypes += ">X"; // We choose the first argument as the async handle
      else if (opts & FuncInfo::COMMAND)
        argTypes += '>';  // Commands always return void - sensible?
      else               
        argTypes += 'Q';  // Other functions return an XLOPER

      // Arg type Q is XLOPER12 values/arrays
      for (auto& arg : info->args)
        argTypes += arg.allowRange ? 'U' : 'Q';

      // Set function option suffixes
      // TODO: check for invalid combinations
      if (opts & FuncInfo::VOLATILE)
        argTypes += '!';
      else if (opts & FuncInfo::MACRO_TYPE)
        argTypes += '#';
      else if (opts & FuncInfo::THREAD_SAFE)
        argTypes += '$';

      // Concatenate argument names, adding optional indicator if required
      wstring argNames;
      for (auto x : info->args)
        if (x.optional)
          argNames.append(formatStr(L"[%s],", x.name.c_str()));
        else
          argNames.append(x.name).append(L",");

      if (numArgs > 0)
        argNames.pop_back();  // Delete final comma

      const bool truncatedArgNames = argNames.length() > 255;
      if (truncatedArgNames)
      {
        XLO_INFO(L"Excel does not support a concatenated argument name length of "
          "more than 255 chars (including commans). Truncating for function '{0}'", info->name);
        argNames.resize(255);
      }

      // Build argument help strings. If we had to truncate the arg name string
      // add the arg names to the argument help string
      vector<wstring> argHelp;
      if (truncatedArgNames)
        for (auto x : info->args)
          argHelp.emplace_back(fmt::format(L"({0}) {1}", x.name, x.help));
      else
        for (auto x : info->args)
          argHelp.emplace_back(x.help);

      // Pad the last arg help with a couple of spaces to workaround an Excel bug
      if (numArgs > 0 && !argHelp.back().empty())
        argHelp.back() += L"  ";

      // Truncate argument help strings to 255 chars
      for (auto& h : argHelp)
        if (h.size() > 255)
        {
          XLO_INFO(L"Excel does not support argument help strings longer than 255 chars. "
            "Truncating for function '{0}'", info->name);
          h.resize(255);
        }

      // Set the function type
      int macroType = 1;
      if (opts & FuncInfo::COMMAND)
        macroType = 2;
      else if (opts & FuncInfo::HIDDEN)
        macroType = 0;

      // Function help string. Yup, more 255 char limits, those MS folks are terse
      auto truncatedHelp = info->help;
      if (info->help.length() > 255)
      {
        XLO_INFO(L"Excel does not support help strings longer than 255 chars. "
          "Truncating for function '{0}'", info->name);
        truncatedHelp.assign(info->help.c_str(), 255);
        truncatedHelp[252] = '.'; truncatedHelp[253] = '.'; truncatedHelp[254] = '.';
      }

      // TODO: this copies the excelobj
      XLO_DEBUG(L"Registering \"{0}\" at entry point {1} with {2} args", 
        info->name, utf8ToUtf16(entryPoint), numArgs);

      auto registerId = callExcel(xlfRegister,
        moduleName, 
        entryPoint, 
        argTypes, 
        info->name, 
        argNames,
        macroType, 
        info->category, 
        nullptr, nullptr, 
        truncatedHelp.empty() ? info->help : truncatedHelp,
        unpack(argHelp));
      if (registerId.type() != ExcelType::Num)
        XLO_THROW(L"Register '{0}' failed", info->name);
      return registerId.toInt();
    }

    void throwIfPresent(const wstring& name) const
    {
      if (theRegistry.find(name) != theRegistry.end())
        XLO_THROW(L"Function {0} already registered", name);
    }

  public:
    RegisteredFuncPtr add(const shared_ptr<const FuncSpec>& spec)
    {
      auto& name = spec->info()->name;
      throwIfPresent(name);

      return theRegistry.emplace(name, spec->registerFunc()).first->second;
    }

    bool remove(const shared_ptr<RegisteredFunc>& func)
    {
      if (func->deregister())
      {
        theRegistry.erase(func->info()->name);
        return true;
      }
      return false;
    }

    void clear()
    {
      for (auto f : theRegistry)
        const_cast<RegisteredFunc&>(*f.second).deregister();
      theRegistry.clear();
      theCodePtr = theCodeCave;
    }

    auto find(const wchar_t* name)
    {
      auto found = theRegistry.find(name);
      return found != theRegistry.end() ? found->second : RegisteredFuncPtr();
    }

  private:
    FunctionRegistry()
    {
      theCoreDllName = ExcelObj(State::coreName());
      theExportTable.reset(new DllExportTable((HMODULE)State::coreModuleHandle()));
      theFirstStub = theExportTable->findOffset(
        decorateCFunction(XLOIL_STUB_NAME_STR, 0).c_str());
      if (theFirstStub < 0)
        XLO_THROW("Could not find xlOil stub");
    }

    map<wstring, RegisteredFuncPtr> theRegistry;
    unique_ptr<DllExportTable> theExportTable;
    size_t theFirstStub;
  };

  char FunctionRegistry::theCodeCave[theCaveSize];
  char* FunctionRegistry::theCodePtr = theCodeCave;


  RegisteredFunc::RegisteredFunc(const shared_ptr<const FuncSpec>& spec)
    : _spec(spec)
  {}

  RegisteredFunc::~RegisteredFunc()
  {
    deregister();
  }

  bool RegisteredFunc::deregister()
  {
    if (_registerId == 0)
      return false;

    auto& name = info()->name;
    XLO_DEBUG(L"Deregistering {0}", name);

    auto[result, ret] = tryCallExcel(xlfUnregister, double(_registerId));
    if (ret != msxll::xlretSuccess || result.type() != ExcelType::Bool || !result.toBool())
    {
      XLO_WARN(L"Unregister failed for {0}", name);
      return false;
    }

    // Cunning trick to workaround SetName where function is not removed from wizard
    // by registering a hidden function (i.e. a command) then removing it.  It 
    // doesn't matter which entry point we bind to as long as the function pointer
    // won't be registered as an Excel func.
    // https://stackoverflow.com/questions/15343282/how-to-remove-an-excel-udf-programmatically

    // SetExcel12EntryPt is automatically created by xlcall.cpp, but is only used for
    // clusters, which we aren't supporting at this current time.
    auto arbitraryFunction = decorateCFunction("SetExcel12EntryPt", 1);
    auto[tempRegId, retVal] = tryCallExcel(
      xlfRegister, FunctionRegistry::get().theCoreDllName, arbitraryFunction.c_str(), "I", name, nullptr, 2);
    tryCallExcel(xlfSetName, name); // SetName with no arg un-sets the name
    tryCallExcel(xlfUnregister, tempRegId);
    _registerId = 0;
    
    return true;
  }

  int RegisteredFunc::registerId() const
  {
    return _registerId;
  }

  const std::shared_ptr<const FuncInfo>& RegisteredFunc::info() const
  {
    return _spec->info();
  }
  const std::shared_ptr<const FuncSpec>& RegisteredFunc::spec() const
  {
    return _spec;
  }
  bool RegisteredFunc::reregister(const std::shared_ptr<const FuncSpec>& /*other*/)
  {
    return false;
  }

  class RegisteredStatic : public RegisteredFunc
  {
  public:
    RegisteredStatic(const std::shared_ptr<const StaticSpec>& spec)
      : RegisteredFunc(spec)
    {
      auto& registry = FunctionRegistry::get();
      _registerId = registry.registerWithExcel(
        spec->info(), 
        decorateCFunction(spec->_entryPoint.c_str(), spec->info()->numArgs()).c_str(), 
        ExcelObj(spec->_dllName));
    }
  };

  template <class TCallback>
  class RegisteredCallback : public RegisteredFunc
  {
  public:
    RegisteredCallback(
      const std::shared_ptr<const GenericCallbackSpec<TCallback>>& spec)
      : RegisteredFunc(spec)
    {
      auto& registry = FunctionRegistry::get();
      auto[thunk, thunkSize] = registry.callBuildThunk(
        spec->_callback, spec->_context.get(), spec->info()->numArgs());
      _thunk = thunk;
      _thunkSize = thunkSize;
      _registerId = doRegister();
    }

    int doRegister() const
    {
      auto& registry = FunctionRegistry::get();
      auto entryPoint = registry.hookEntryPoint(*info(), _thunk);
      return registry.registerWithExcel(info(), entryPoint.c_str(), registry.theCoreDllName);
    }

    virtual bool reregister(const std::shared_ptr<const FuncSpec>& other)
    {
      auto* thisType = dynamic_cast<const GenericCallbackSpec<TCallback>*>(other.get());
      if (!thisType)
        return false;

      auto& newInfo = other->info();
      auto newContext = thisType->_context;
      auto& context = spec()._context;

      XLO_ASSERT(info()->name == newInfo->name);
      if (_thunk && info()->numArgs() == newInfo->numArgs() && info()->options == newInfo->options)
      {
        bool infoMatches = *info() == *newInfo;
        bool contextMatches = context != newContext;

        if (!contextMatches)
        {
          XLO_DEBUG(L"Patching function context for '{0}'", newInfo->name);
          if (!patchThunkData((char*)_thunk, _thunkSize, context.get(), newContext.get()))
          {
            XLO_ERROR(L"Failed to patch context for '{0}'", newInfo->name);
            return false;
          }
        }
        // If the FuncInfo is identical, no need to re-register, note this
        // discards the new funcinfo.
        if (infoMatches)
          return true;

        // Rewrite spec
        _spec = make_shared<GenericCallbackSpec<TCallback>>(newInfo, spec()._callback, newContext);

        // Otherwise re-use the possibly patched thunk
        XLO_DEBUG(L"Reregistering function '{0}'", newInfo->name);
        deregister();
        _registerId = doRegister();
        _spec = other;
        return true;
      }
      return false;
    }

    const GenericCallbackSpec<TCallback>& spec() const
    {
      return static_cast<const GenericCallbackSpec<TCallback>&>(*_spec);
    }

  private:
    void* _thunk;
    size_t _thunkSize;
  };

  std::shared_ptr<RegisteredFunc> StaticSpec::registerFunc() const
  {
    return make_shared<RegisteredStatic>(
      std::static_pointer_cast<const StaticSpec>(this->shared_from_this()));
  }

  std::shared_ptr<RegisteredFunc> GenericCallbackSpec<RegisterCallback>::registerFunc() const
  {
    return make_shared<RegisteredCallback<RegisterCallback>>(
      std::static_pointer_cast<const GenericCallbackSpec<RegisterCallback>>(this->shared_from_this()));
  }

  std::shared_ptr<RegisteredFunc> GenericCallbackSpec<AsyncCallback>::registerFunc() const
  {
    return make_shared<RegisteredCallback<AsyncCallback>>(
      std::static_pointer_cast<const GenericCallbackSpec<AsyncCallback>>(this->shared_from_this()));
  }

  namespace
  {
    ExcelObj* launchFunctionObj(
      LambdaFuncSpec* data, 
      const ExcelObj** args) noexcept
    {
      try
      {
        return data->_function(*data->info(), args);
      }
      catch (const std::exception& e)
      {
        return returnValue(e);
      }
    }

    // TODO: this is not used and maybe not that useful!

    class AsyncHolder
    {
    public:
      // No need to copy the data as FuncRegistry will keep this alive
      // Async handle is destroyed by Excel return, so must copy that
      AsyncHolder(std::function<ExcelObj*()> func, const ExcelObj* asyncHandle)
        : _call(func)
        , _asyncHandle(*asyncHandle)
      {
      }
      void operator()(int /*threadId*/) const
      {
        auto* result = _call();
        asyncReturn(_asyncHandle, ExcelObj(*result));
        if (result->xltype & msxll::xlbitDLLFree)
          delete result;
      }
    private:
      std::function<ExcelObj*()> _call;
      ExcelObj _asyncHandle;
    };

    void launchFunctionObjAsync(
      LambdaFuncSpec* data, 
      const ExcelObj* asyncHandle, 
      const ExcelObj** args) noexcept
    {
      try
      {
        auto nArgs = data->info()->numArgs();

        // Make a shared_ptr so the lambda below can capture it without a copy
        auto argsCopy = make_shared<vector<ExcelObj>>();
        argsCopy->reserve(nArgs);
        std::transform(args, args + nArgs, std::back_inserter(*argsCopy), [](auto* p) {return ExcelObj(*p); });

        auto functor = AsyncHolder(
          [argsCopy, data]()
          {
            std::vector<const ExcelObj*> argsPtr;
            argsPtr.reserve(argsCopy->size());
            std::transform(argsCopy->begin(), argsCopy->end(), std::back_inserter(argsPtr), [](ExcelObj& x) { return &x; });
            return data->_function(*data->info(), &argsPtr[0]);
          }, 
          asyncHandle);

        //Very simple with no cancellation
        std::thread go(functor, 0);
        go.detach();
      }
      catch (...)
      {
      }
    }
  }

  std::shared_ptr<RegisteredFunc> LambdaFuncSpec::registerFunc() const
  {
    auto copyThis = make_shared<LambdaFuncSpec>(*this);
    if ((info()->options & FuncInfo::ASYNC) != 0)
      return AsyncCallbackSpec(info(), &launchFunctionObjAsync, copyThis).registerFunc();
    else
      return CallbackSpec(info(), &launchFunctionObj, copyThis).registerFunc();
  }

  RegisteredFuncPtr registerFunc(const std::shared_ptr<const FuncSpec>& spec) noexcept
  {
    try
    {
      return FunctionRegistry::get().add(spec);
    }
    catch (std::exception& e)
    {
      XLO_ERROR("Failed to register func {0}: {1}",
        utf16ToUtf8(spec->info()->name.c_str()), e.what());
      return RegisteredFuncPtr();
    }
  }

  RegisteredFuncPtr findRegisteredFunc(const wchar_t * name)
  {
    return FunctionRegistry::get().find(name);
  }
 
  bool deregisterFunc(const shared_ptr<RegisteredFunc>& ptr)
  {
    return FunctionRegistry::get().remove(ptr);
  }

  StaticFunctionSource::StaticFunctionSource(const wchar_t* pluginPath)
    : FileSource(pluginPath)
  {
    // This collects all statically declared Excel functions, i.e. raw C functions
    // It assumes that this ctor and hence processRegistryQueue is run after each
    // plugin has been loaded, so that all functions on the queue belong to the 
    // current plugin
    auto specs = processRegistryQueue(pluginPath);
    registerFuncs(specs);
  }

  namespace
  {
    struct RegisterMe
    {
      RegisterMe()
      {
        // TODO: all funcs *should* be removed by this point - check this
        static auto handler = Event::AutoClose() += []() { FunctionRegistry::get().clear(); };
      }
    } theInstance;
  }
}