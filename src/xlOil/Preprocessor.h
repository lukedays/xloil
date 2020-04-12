#pragma once
#include <boost/preprocessor/repeat_from_to.hpp>
#include <boost/preprocessor/arithmetic.hpp>
#include <boost/preprocessor/repetition/enum_shifted_params.hpp>
#include <boost/preprocessor/tuple.hpp>

namespace xloil { class ExcelObj; }


/// <summary>
/// Stringifies the given argument to a wide string literal
/// </summary>
#define XLO_WSTR(s) L ## #s
#define XLO_STR_IMPL(s) #s

/// <summary>
///  Stringifies the given argument to a narrow string literal
/// </summary>
#define XLO_STR(s) XLO_STR_IMPL(s)

/// <summary>
/// Use in a function declaration to declare <code>const ExcelObj&</code> arguments 
/// named prefix1, prefix2, ..., prefixN
/// </summary>
#define XLO_DECLARE_ARGS(N, prefix) BOOST_PP_ENUM_SHIFTED_PARAMS(BOOST_PP_ADD(N,1), const ExcelObj& prefix)

/// <summary>
/// Returns a comma-separated list of argument addresses: &prefix1, &prefix2, ..., &prefixN.
/// Useful to create an array of function arguments.
/// </summary>
#define XLO_ARG_PTRS(N, prefix) BOOST_PP_ENUM_SHIFTED_PARAMS(BOOST_PP_ADD(N,1), &prefix)

/// <summary>
/// Returns a list of argument values and their names for use with <see cref="xloil::ProcessArgs"/>.
/// </summary>
#define XLO_ARGS_LIST(N, prefix) BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(1, N), XLO_ARGS_LIST_IMPL, prefix)
#define XLO_ARGS_LIST_IMPL(z, N, prefix) BOOST_PP_COMMA_IF(BOOST_PP_SUB(N, 1)) prefix##N, XLO_WSTR(prefix##N) 


namespace xloil
{
  class ExcelObj;

  /// <summary>
  /// Iterates over a number of ExcelObj arguments, applying a function to each.
  /// Best illustrated with an example:
  /// 
  /// <example>
  ///   ProcessArgs([&str](auto iArg, auto argVal, auto argName)
  ///   {
  ///      str += wstring(argName) + ": " + argVal.toString() + "\n";
  ///   }, XLO_ARGS_LIST(8, arg));
  /// </example>
  /// 
  /// ProcessArgs will accept lambdas which do not contain the <code>Arg</code>
  /// or <code>argName</code> arguments.
  ///  
  /// </summary>
  template<int N = 0, class TFunc, class... Args>
  auto ProcessArgs(TFunc func, const ExcelObj& argVal, const wchar_t* argName, Args...args)
    -> decltype(func(N, argVal, argName))
  {
    func(N, argVal, argName);
    ProcessArgs<N + 1>(func, args...);
  }
  template<int N, class TFunc>
  auto ProcessArgs(TFunc func, const ExcelObj& argVal, const wchar_t* argName)
    -> decltype(func(N, argVal, argName))
  {
    func(N, argVal, argName);
  }

  template<class TFunc, class... Args>
  auto ProcessArgs(TFunc func, const ExcelObj& argVal, const wchar_t* argName, Args...args)
    -> decltype(func(argVal, argName))
  {
    func(argVal, argName);
    ProcessArgs(func, args...);
  }

  template<class TFunc>
  auto ProcessArgs(TFunc func, const ExcelObj& argVal, const wchar_t* argName)
    -> decltype(func(argVal, argName))
  {
    func(argVal, argName);
  }

  template<class TFunc, class... Args>
  auto ProcessArgs(TFunc func, const ExcelObj& argVal, const wchar_t* argName, Args...args)
    -> decltype(func(argVal))
  {
    func(argVal);
    ProcessArgs(func, args...);
  }

  template<class TFunc>
  auto ProcessArgs(TFunc func, const ExcelObj& argVal, const wchar_t* argName)
    -> decltype(func(argVal))
  {
    func(argVal);
  }
}
