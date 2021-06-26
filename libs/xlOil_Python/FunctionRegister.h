#pragma once
#include "PyCoreModule.h"
#include <map>
#include <string>
#include <pybind11/pybind11.h>

namespace xloil {
  class AddinContext; 
  struct FuncInfo; 
  class ExcelObj; 
  template <class T> class IConvertFromExcel;
}
namespace xloil 
{
  namespace Python
  {
    class RegisteredModule;
    using IPyFromExcel = IConvertFromExcel<PyObject*>;

    namespace FunctionRegistry
    {
      /// <summary>
      /// Adds the specified module to the specified context if the module
      /// has not already been read. If the module already exists, just 
      /// returns a reference to it.
      /// </summary>
      std::shared_ptr<RegisteredModule>
        addModule(
          AddinContext* context, 
          const std::wstring& modulePath,
          const wchar_t* workbookName);
    };

    class PyFuncArg
    {
    private:
      std::shared_ptr<FuncInfo> _info;
      unsigned _argNum;
      pybind11::object _default;

    public:
      PyFuncArg(std::shared_ptr<FuncInfo> info, unsigned i)
        : _info(info)
        , _argNum(i)
        , arg(_info->args[i])
      {}

      FuncArg& arg;

      std::shared_ptr<IPyFromExcel> converter;
      
      void setName(const std::wstring& value) { arg.name = value; }
      const auto& getName() const { return arg.name; }

      void setHelp(const std::wstring& value) { arg.help = value; }
      const auto& getHelp() const { return arg.help; }

      void setDefault(const pybind11::object& value) 
      {
        arg.type |= FuncArg::Optional;
        _default = value; 
      }
      auto getDefault() const 
      { 
        // what to return if this is null???
        return _default; 
      }
    };

    class PyFuncInfo
    {
    public:
      PyFuncInfo(
        const std::wstring& name,
        const pybind11::function& func,
        const unsigned numArgs,
        const std::string& features,
        const std::wstring& help,
        const std::wstring& category,
        bool isLocal,
        bool isVolatile,
        bool hasKeywordArgs);
      
      ~PyFuncInfo();

      auto& args() { return _args; }

      void setFuncOptions(unsigned val);

      auto getReturnConverter() const { return returnConverter; }
      void setReturnConverter(
        const std::shared_ptr<const IPyToExcel>& conv);

      std::pair<pybind11::tuple, pybind11::object> convertArgs(
        const ExcelObj** xlArgs) const;

      void invoke(
        ExcelObj& result, PyObject* args, PyObject* kwargs) const noexcept;

      void invoke(
        PyObject* args, PyObject* kwargs) const;

      bool isLocalFunc;
      bool isAsync;
      bool isRtdAsync;
      bool isThreadSafe() const { return (_info->options & FuncInfo::THREAD_SAFE) != 0; }

      std::shared_ptr<const FuncInfo>  info() const { return _info; }

    private:
      std::shared_ptr<const IPyToExcel> returnConverter;
      std::vector<PyFuncArg> _args;
      std::shared_ptr<FuncInfo> _info;
      pybind11::function _func;
      bool _hasKeywordArgs;
    };
  }
}