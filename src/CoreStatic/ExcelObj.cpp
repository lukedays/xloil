#include "ExcelObj.h"
#include "xloil/ExcelCall.h"
#include "StandardConverters.h"
#include "xloil/Log.h"
#include "xloil/Date.h"
#include "xloil/Utils.h"
#include "ExcelArray.h"
#include <algorithm>
#include <cstring>
#include <vector>
#include <string>
#include <codecvt>

#define MAX_XL11_ROWS            65536
#define MAX_XL11_COLS              256
#define MAX_XL12_ROWS          1048576
#define MAX_XL12_COLS            16384
#define MAX_XL11_UDF_ARGS           30
#define MAX_XL12_UDF_ARGS          255
#define MAX_XL4_STR_LEN           255u
#define MAX_XL12_STR_LEN        32767ul

using std::string;
using std::wstring;
using std::vector;
using namespace msxll;

//class ExcelObjectSetType : protected xloper12
//{
//protected:
//  ExcelObjectSetType()
//  {
//    xltype = xltypeNil;
//  }
//};
namespace xloil
{
namespace
{
  static_assert(sizeof(xloper12) == sizeof(xloil::ExcelObj));

  wchar_t* makeStringBuffer(size_t& nChars)
  {
    nChars = std::min<size_t>(nChars, MAX_XL12_STR_LEN - 2);
    auto buf = new wchar_t[nChars + 2];
    buf[0] = (wchar_t)nChars;
    buf[nChars + 1] = L'\0';
    return buf + 1;
  }

  wchar_t* pascalWStringFromC(const char* s)
  {
    assert(s);

    size_t len = strlen(s);
    auto wideStr = makeStringBuffer(len);

    size_t nChars = 0;
    mbstowcs_s(&nChars, wideStr, len + 1, s, len);

    return wideStr - 1;
  }
  wchar_t* pascalWStringFromC(const wchar_t* s)
  {
    assert(s);
    auto len = wcslen(s);
    auto wideStr = makeStringBuffer(len);
    // no need to copy null-terminator as makeStringBuffer creates it
    wmemcpy_s(wideStr, len, s, len); 
    return wideStr - 1;
  }

  size_t totalStringLength(xloper12* arr, size_t nRows, size_t nCols)
  {
    size_t total = 0;
    auto endData = arr + (nRows * nCols);
    for (; arr != endData; ++arr)
      if (arr->xltype == xltypeStr)
        total += arr->val.str[0];
    return total;
  }

  void overwrite(ExcelObj& to, const ExcelObj& from)
  {
    // TODO: can't we memcpy the simple types here?
    switch (from.xltype & ~(xlbitXLFree | xlbitDLLFree))
    {
    case xltypeNum:
      to.val.num = from.val.num;
      to.xltype = xltypeNum;
      break;

    case xltypeBool:
      to.val.xbool = from.val.xbool;
      to.xltype = xltypeBool;
      break;

    case xltypeErr:
      to.xltype = xltypeErr;
      to.val.err = from.val.err;
      break;

    case xltypeMissing:
    case xltypeNil:
      to.xltype = from.xltype;
      break;

    case xltypeInt:
      to.xltype = xltypeInt;
      to.val.w = from.val.w;
      break;

    case xltypeStr:
    {
      size_t len = from.val.str[0];
      to.val.str = new wchar_t[len + 2];
      wmemcpy_s(to.val.str, len + 1, from.val.str, len + 1);
      to.val.str[len + 1] = L'\0';  // Allows debugger to read string
      to.xltype = xltypeStr;
      break;
    }
    case xltypeMulti:
    {
      auto nRows = from.val.array.rows;
      auto nCols = from.val.array.columns;
      auto size = nRows * nCols;

      auto pSrc = from.val.array.lparray;

      size_t strLength = totalStringLength(pSrc, nRows, nCols);
      ExcelArrayBuilder arr(nRows, nCols, strLength, false);

      for (auto i = 0; i < nRows; ++i)
        for (auto j = 0; j < nCols; ++j)
        {
          switch (pSrc->xltype)
          {
          case xltypeStr:
          {
            wchar_t* buf;
            size_t len = pSrc->val.str[0];
            arr.emplace_at(i, j, buf, len);
            wmemcpy_s(buf, len, pSrc->val.str + 1, len);
            break;
          }
          default:
            arr.emplace_at(i, j, *(ExcelObj*)pSrc);
          }
          ++pSrc;
        }

      to.val.array.lparray = (XLOIL_XLOPER*)arr.at(0, 0);
      to.val.array.rows = nRows;
      to.val.array.columns = nCols;
      to.xltype = xltypeMulti;
      break;
    }
    case xltypeBigData:
    {
      auto cbData = from.val.bigdata.cbData;

      // Either it's a block of data to copy or a handle from Excel
      if (cbData > 0 && from.val.bigdata.h.lpbData)
      {
        auto pbyte = new char[cbData];
        memcpy_s(pbyte, cbData, from.val.bigdata.h.lpbData, cbData);
        to.val.bigdata.h.lpbData = (BYTE*)pbyte;
      }
      else
        to.val.bigdata.h.hdata = from.val.bigdata.h.hdata;

      to.val.bigdata.cbData = cbData;
      to.xltype = xltypeBigData;

      break;
    }
    default:
      XLO_THROW("Unhandled xltype during copy");
    }
  }
}

  // TODO: https://stackoverflow.com/questions/52737760/how-to-define-string-literal-with-character-type-that-depends-on-template-parame
  const wchar_t* toWCString(CellError e)
  {
    switch (e)
    {
    case CellError::Null: return L"#NULL";
    case CellError::Div0: return L"#DIV/0";
    case CellError::Value: return L"#VALUE!";
    case CellError::Ref: return L"#REF!";
    case CellError::Name: return L"#NAME?";
    case CellError::Num: return L"#NUM!";
    case CellError::NA: return L"#N/A";
    case CellError::GettingData: 
    default:
      return L"#ERR!";
    }
  }

  enum class Alloc
  {
    BLOB,
    ARRAY,
    NONE
  };

  ExcelObj::ExcelObj()
  {
    xltype = xltypeNil;
  }

  ExcelObj::ExcelObj(int i)
  {
    xltype = xltypeInt;
    val.w = i;
  }

  ExcelObj::ExcelObj(double d)
  {
    xltype = xltypeNum;
    val.num = d;
  }

  ExcelObj::ExcelObj(bool b)
  {
    xltype = xltypeBool;
    val.xbool = b ? 1 : 0;
  }
  ExcelObj::ExcelObj(ExcelType t)
  {
    switch (t)
    {
    case ExcelType::Num: val.num = 0; break;
    case ExcelType::Int: val.w = 0; break;
    case ExcelType::Bool: val.xbool = 0; break;
    case ExcelType::Str: val.str = Const::EmptyStr().val.str; break;
    case ExcelType::Err: val.err = (int)CellError::NA; break;
    case ExcelType::Multi: val.array.rows = 0; val.array.columns = 0; break;
    case ExcelType::SRef:
    case ExcelType::Flow:
    case ExcelType::BigData:
      XLO_THROW("Flow and SRef and BigData types not supported");
    }
    xltype = int(t);
  }
  ExcelObj::ExcelObj(const PString<wchar_t>& pstr)
  {
    val.str = const_cast<wchar_t*>(pstr.buf());
    xltype = xltypeStr;
  }

  ExcelObj::ExcelObj(const char* s)
  {
    val.str = pascalWStringFromC(s);
    xltype = xltypeStr;
  }

  ExcelObj::ExcelObj(const wchar_t* s)
  {
    if (s == nullptr)
      val.str = Const::EmptyStr().val.str;
    else
      val.str = pascalWStringFromC(s);

    xltype = xltypeStr;
  }

  ExcelObj::ExcelObj(const std::string& s)
    :ExcelObj(s.c_str())
  {
  }

  ExcelObj::ExcelObj(const std::wstring& s)
    :ExcelObj(s.c_str())
  {
  }

  ExcelObj::ExcelObj(nullptr_t)
  {
    xltype = xltypeMissing;
  }

  ExcelObj::ExcelObj(const ExcelObj & that)
  {
    overwrite(*this, that);
  }

  ExcelObj::ExcelObj(ExcelObj&& that)
  {
    // Steal all data
    this->val = that.val;
    this->xltype = that.xltype;
    // Mark donor object as empty
    that.xltype = xltypeNil;
  }

  ExcelObj::ExcelObj(CellError err)
  {
    val.err = (int)err;
    xltype = xltypeErr;
  }

  ExcelObj::ExcelObj(const ExcelObj* array, int nRows, int nCols)
  {
    val.array.rows = nRows;
    val.array.columns = nCols;
    val.array.lparray = (XLOIL_XLOPER*)array;
    xltype = xltypeMulti;
  }

  void ExcelObj::copy(ExcelObj& to, const ExcelObj& from)
  {
    to.reset();
    overwrite(to, from);
  }

  ExcelObj & ExcelObj::fromExcel()
  {
    xltype |= xlbitXLFree;
    return *this;
  }

  ExcelObj * ExcelObj::toExcel()
  {
    xltype |= xlbitDLLFree;
    return this;
  }

  ExcelType ExcelObj::type() const
  {
    return ExcelType(xtype());
  }

  int ExcelObj::xtype() const
  {
    return xltype & ~(xlbitXLFree | xlbitDLLFree);
  }

  ExcelObj::ExcelObj(wchar_t*& outBuffer, size_t& nChars)
  {
    outBuffer = makeStringBuffer(nChars);
    val.str = outBuffer - 1;
    xltype = xltypeStr;
  }

  ExcelObj::~ExcelObj()
  {
    reset();
  }

  void ExcelObj::reset()
  {
      if ((xltype & xlbitXLFree) != 0)
      {
        const auto* p = this;
        callExcelRaw(xlFree, this, 1, &p); // TODO: so arg is const eh?
      }
      else
      {
        switch (xtype())
        {
        case xltypeStr:
          if (val.str != nullptr && val.str != Const::EmptyStr().val.str)
            delete[] val.str;
          break;

        case xltypeMulti:
          // Arrays are allocated as an array of char which contains all their strings
          // So we don't need to loop and free them individually
          delete[](char*)(val.array.lparray);
          break;

        case xltypeBigData: break;
          //TODO: Not implemented yet, we don't create our own bigdata
        }
      }

      xltype = xltypeErr;
      val.err = xlerrNA;
  }
  ExcelObj & ExcelObj::operator=(const ExcelObj & that)
  {
    if (this == &that)
      return *this;
    copy(*this, that);
    return *this;
  }
  ExcelObj & ExcelObj::operator=(ExcelObj&& that)
  {
    reset();
    this->val = that.val;
    this->xltype = that.xltype;
    that.xltype = xltypeNil;
    return *this;
  }
  bool ExcelObj::operator==(const ExcelObj& that) const
  {
    return compare(*this, that) == 0;
  }

  namespace
  {
    template<class T> int cmp(T l, T r)
    {
      return l < r ? -1 : (l == r ? 0 : 1);
    }
  }
  int ExcelObj::compare(const ExcelObj & left, const ExcelObj & right)
  {
    auto lType = left.xtype();
    auto rType = right.xtype();
    // Not sure how best to handle the different type case. Attempt to coerce?
    if (lType != rType)
      return INT_MIN;

    switch (lType)
    {
    case xltypeNum:
      return cmp(left.val.num, right.val.num);
    case xltypeBool:
      return cmp(left.val.xbool, right.val.xbool);
    case xltypeInt:
      return cmp(left.val.w, right.val.w);
    case xltypeErr:
      return cmp(left.val.err, right.val.err);
    case xltypeStr:
    {
      auto lLen = left.val.str[0];
      auto rLen = right.val.str[0];
      auto c = memcmp(left.val.str + 1, right.val.str + 1, std::min(lLen, rLen));
      return c != 0 ? c : cmp(lLen, rLen);
    }
    case xltypeMissing:
    case xltypeNil:
      return 0;
    default:
      return 0; // Not sure this is a very sensible default?
    }
  }
  bool ExcelObj::isMissing() const
  {
    return (xtype() & xltypeMissing) != 0;
  }
  bool ExcelObj::isNonEmpty() const
  {
    switch (xtype())
    {
    case xltypeErr:
      return val.err != xlerrNA;
    case xltypeMissing:
    case xltypeNil:
      return false;
    case xltypeStr:
      return val.str[0] != L'\0';
    default:
      return true;
    }
  }

  std::wstring ExcelObj::toString(bool strict) const
  {
    if (strict && (xltype & xltypeStr) == 0)
      XLO_THROW("Not a string");

    switch (xtype())
    {
    case xltypeNum:
      return std::to_wstring(val.num);

    case xltypeBool:
      return wstring(val.xbool ? L"TRUE" : L"FALSE");

    case xltypeInt:
      return std::to_wstring(val.w);
      break;

    case xltypeStr:
    {
      size_t len = val.str[0];
      return wstring(val.str + 1, len);
    }

    case xltypeMissing:
    case xltypeNil:
      return L"";

    case xltypeErr:
      return toWCString(CellError(val.err));

   // case xltypeRef:
    case xltypeSRef:
    {
      size_t len = 30;
      auto buf = wstring(len, L'\0');
      len = xlrefToString(val.sref.ref, (wchar_t*)buf.data(), len);
      buf.resize(len);
      return buf;
    }
    default:
      return L"#???";
    }
  }

  double ExcelObj::toDouble() const
  {
    return FromExcel<ToDouble>()(*this);
  }

  int ExcelObj::toInt() const
  {
    return FromExcel<ToInt>()(*this);
  }

  bool ExcelObj::toBool() const
  {
    return FromExcel<ToBool>()(*this);
  }

  double ExcelObj::asDouble() const
  {
    assert(xtype() == xltypeNum);
    return val.num;
  }

  int ExcelObj::asInt() const
  {
    assert(xtype() == xltypeInt);
    return val.w;
  }

  bool ExcelObj::asBool() const
  {
    assert(xtype() == xltypeBool);
    return val.xbool;
  }

  const wchar_t * ExcelObj::asPascalStr(size_t & length) const
  {
    if ((xtype() & xltypeStr) == 0)
    {
      length = 0;
      return nullptr;
    }

    length = val.str[0];
    return val.str + 1;
  }

  size_t ExcelObj::writeString(wchar_t* buf, size_t bufSize) const
  {
    size_t len;
    auto s = this->asPascalStr(len);
    if (!s) // We are not a string
      return 0;
    len = std::min(len, bufSize - 1);
    wmemcpy_s(buf, len, s, len);
    buf[len] = L'\0';
    return len;
  }

  bool ExcelObj::toDMY(int &nDay, int &nMonth, int &nYear, bool coerce)
  {
    auto d = toInt();
    return excelSerialDateToDMY(d, nDay, nMonth, nYear);
  }

  bool ExcelObj::toDMYHMS(int & nDay, int & nMonth, int & nYear, int & nHours, int & nMins, int & nSecs, int & uSecs, bool coerce)
  {
    auto d = toDouble();
    return excelSerialDatetoDMYHMS(d, nDay, nMonth, nYear, nHours, nMins, nSecs, uSecs);
  }

  bool ExcelObj::trimmedArraySize(int& nRows, int& nCols) const
  {
    if ((xtype() & xltypeMulti) == 0)
    {
      nRows = 0; nCols = 0;
      return false;
    }

    auto start = (ExcelObj*)val.array.lparray;
    nRows = val.array.rows;
    nCols = val.array.columns;

    auto p = start + nCols * nRows - 1;

    for (; nRows > 0; --nRows)
      for (int c = nCols - 1; c >= 0; --c, --p)
        if (p->isNonEmpty())
          goto StartColSearch;

  StartColSearch:
    for (; nCols > 0; --nCols)
      for (p = start + nCols; p <= (start + nCols * nRows); p += val.array.columns)
        if (p->isNonEmpty())
          goto SearchDone;

  SearchDone:
    return true;
  }

  // Uses RxCy format as it's easier for the programmer!
  size_t xlrefToString(const XLREF12& ref, wchar_t* buf, size_t bufSize)
  {
    // Add one everywhere here as rwFirst is zero-based but RxCy format is 1-based
    if (ref.rwFirst == ref.rwLast && ref.colFirst == ref.colLast)
      return _snwprintf_s(buf, bufSize, bufSize, L"R%dC%d", ref.rwFirst + 1, ref.colFirst + 1);
    else
      return _snwprintf_s(buf, bufSize, bufSize, L"R%dC%d:R%dC%d", ref.rwFirst + 1, ref.colFirst + 1, ref.rwLast + 1, ref.colLast + 1);
  }

  namespace Const
  {
    const ExcelObj& Missing()
    {
      static ExcelObj obj = ExcelObj(ExcelType::Missing);
      return obj;
    }

    const ExcelObj& Error(CellError e)
    {
      static std::array<ExcelObj, theCellErrors.size()> cellErrors =
      {
        ExcelObj(CellError::Null),
        ExcelObj(CellError::Div0),
        ExcelObj(CellError::Value),
        ExcelObj(CellError::Ref),
        ExcelObj(CellError::Name),
        ExcelObj(CellError::Num),
        ExcelObj(CellError::NA),
        ExcelObj(CellError::GettingData)
      };
      switch (e)
      {
      case CellError::Null:        return cellErrors[0];
      case CellError::Div0:        return cellErrors[1];
      case CellError::Value:       return cellErrors[2];
      case CellError::Ref:         return cellErrors[3];
      case CellError::Name:        return cellErrors[4];
      case CellError::Num:         return cellErrors[5];
      case CellError::NA:          return cellErrors[6];
      case CellError::GettingData: return cellErrors[7];
      }
      XLO_THROW("Bad thing happened");
    }
    const ExcelObj& EmptyStr()
    {
      static ExcelObj obj(PString<wchar_t>(L'\0'));
      return obj;
    }

  }
}
