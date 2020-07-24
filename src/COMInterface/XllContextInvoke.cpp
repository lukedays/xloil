#include "XllContextInvoke.h"
#include "ExcelTypeLib.h"
#include "Connect.h"
#include <xlOil/ExcelObj.h>
#include "CallHelper.h"
#include <xloil/ThreadControl.h>
#include <xloil/StaticRegister.h>
#include <xloil/ExcelCall.h>
#include <xloil/Log.h>

namespace xloil
{
  static const std::function<void()>* theVoidFunc = nullptr;
  static int theExcelCallFunc = 0;
  static XLOIL_XLOPER* theExcelCallResult = nullptr;
  static XLOIL_XLOPER** theExcelCallArgs = nullptr;
  static int theExcelCallNumArgs = 0;

  XLO_ENTRY_POINT(XLOIL_XLOPER*) xloRunInXLLContext()
  {
    static ExcelObj result(0);
    try
    {
      InXllContext context;
      if (theVoidFunc)
        (*theVoidFunc)();
      else
        result.val.w = Excel12v(theExcelCallFunc, theExcelCallResult, theExcelCallNumArgs, theExcelCallArgs);
    }
    catch (...)
    {
    }
    return &result;
  }
  XLO_REGISTER_FUNC(xloRunInXLLContext)
    .macro().hidden();

  InXllContext::InXllContext()
  {
    ++_count;
  }
  InXllContext::~InXllContext()
  {
    --_count;
  }
  bool InXllContext::check()
  {
    return InComContext::_count > 0 ? false : _count > 0;
  }

  int InXllContext::_count = 0;

  InComContext::InComContext()
  {
    ++_count;
  }
  InComContext::~InComContext()
  {
    --_count;
  }
  bool InComContext::check()
  {
    return !InXllContext::check();
  }

  int InComContext::_count = 0;

  bool runInXllContext(const std::function<void()>& f)
  {
    if (InXllContext::check())
    {
      f();
      return true;
    }

    auto[result, xlret] = tryCallExcel(msxll::xlfGetDocument, 1);
    if (xlret == 0)
    {
      f();
      return true;
    }

    theVoidFunc = &f;

    auto ret = runOnMainThread<std::optional<_variant_t>>([]() 
    {
      return retryComCall([]()
      {
        return excelApp().Run("xloRunFuncInXLLContext");
      });
    });
    return ret.get().has_value();
  }

  int runInXllContext(int func, ExcelObj* result, int nArgs, const ExcelObj** args)
  {
    if (InXllContext::check())
    {
      Excel12v(func, result, nArgs, (XLOIL_XLOPER**)args);
      return true;
    }
    theVoidFunc = nullptr;
    theExcelCallFunc = func;
    theExcelCallResult = result;
    theExcelCallArgs = (XLOIL_XLOPER**)args;
    theExcelCallNumArgs = nArgs;
    //XLO_TRACE("Calling into XLL context fn= {0:#x}", (size_t)&fn);
    auto ret = retryComCall([]()
    { 
      return excelApp().Run("xloRunInXLLContext");
    });
    if (!ret)
      return msxll::xlretInvXlfn;
    auto variant = ret.value();
    
    if (SUCCEEDED(VariantChangeType(&variant, &variant, 0, VT_I4)))
      return variant.lVal;

    return msxll::xlretInvXlfn;
  }
}