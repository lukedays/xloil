#include "Register.h"
#include "ExcelCall.h"
#include "Interface.h"
#include "Events.h"
#include "Log.h"
#include "FuncRegistry.h"
#include <asmjit/src/asmjit/asmjit.h>
#include <codecvt>

using std::vector;
using std::shared_ptr;
using std::unique_ptr;
using std::string;
using std::wstring;
using std::unordered_map;

namespace xloil
{
  
  FuncRegistrationMemo::FuncRegistrationMemo(const char* entryPoint_, size_t nArgs)
    : _nArgs(nArgs)
    , entryPoint(entryPoint_)
    , _info(new FuncInfo())
  {
    // TODO: why aren't we using the function in Utils?
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    _info->name = conv.from_bytes(entryPoint_);
  }

  std::shared_ptr<const FuncInfo> FuncRegistrationMemo::getInfo()
  {
    using namespace std::string_literals;
    
    while (_info->args.size() < _nArgs)
      _info->args.emplace_back(FuncArg((L"Arg_"s + std::to_wstring(_info->args.size() - 1)).c_str()));
    if (_info->args.size() > _nArgs)
      XLO_THROW("Too many args for function");
    if ((_info->options & FuncInfo::ASYNC) != 0)
      _info->args.pop_back(); // TODO: hack!!!!!!!!
    return _info;
  }

  std::list<FuncRegistrationMemo>& getFuncRegistryQueue()
  {
    static std::list<FuncRegistrationMemo> theQueue;
    return theQueue;
  }

  XLOIL_EXPORT FuncRegistrationMemo& createRegistrationMemo(const char* entryPoint_, size_t nArgs)
  {
    getFuncRegistryQueue().emplace_back(entryPoint_, nArgs);
    return getFuncRegistryQueue().back();
  }

  std::vector<RegisteredFuncPtr> processRegistryQueue(const wchar_t* moduleName)
  {
    vector<RegisteredFuncPtr> result;
    auto& q = getFuncRegistryQueue();
    for (auto f : q)
    {
      result.emplace_back(registerFunc(f.getInfo(), f.entryPoint.c_str(), moduleName));
    }
    q.clear();
    return result;
  }
}