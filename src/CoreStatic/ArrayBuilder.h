#pragma once

#include "ExcelObj.h"
#include <cassert>

namespace xloil
{
  class ExcelArrayBuilder
  {
  public:
    ExcelArrayBuilder(size_t nRows, size_t nCols,
      size_t totalStrLength = 0, bool pad2DimArray = false)
    {
      // Add the terminators and string counts to total length
      // Not everything has to be a string so this is an over-estimate
      if (totalStrLength > 0)
        totalStrLength += nCols * nRows * 2;

      auto nPaddedRows = nRows;
      auto nPaddedCols = nCols;
      if (pad2DimArray)
      {
        if (nPaddedRows == 1) nPaddedRows = 2;
        if (nPaddedCols == 1) nPaddedCols = 2;
      }

      auto arrSize = nPaddedRows * nPaddedCols;

      auto* buf = new char[sizeof(ExcelObj) * arrSize + sizeof(wchar_t) * totalStrLength];
      _arrayData = (ExcelObj*)buf;
      _stringData = (wchar_t*)(_arrayData + arrSize);
      _endStringData = _stringData + totalStrLength;
      _nRows = nPaddedRows;
      _nColumns = nPaddedCols;

      // Add padding
      if (nCols < nPaddedCols)
        for (size_t i = 0; i < nRows; ++i)
          emplace_at(i, nCols, CellError::NA);

      if (nRows < nPaddedRows)
        for (size_t j = 0; j < nPaddedCols; ++j)
          emplace_at(nRows, j, CellError::NA);
    }
    int emplace_at(size_t i, size_t j)
    {
      new (at(i, j)) ExcelObj(CellError::NA);
      return 0;
    }
    // TODO: this is lazy, only int, bool, double and ExcelError are supported here, others are UB
    template <class T>
    int emplace_at(size_t i, size_t j, T&& x)
    {
      new (at(i, j)) ExcelObj(std::forward<T>(x));
      return 0;
    }
    int emplace_at(size_t i, size_t j, wchar_t*& buf, size_t& len)
    {
      buf = _stringData + 1;
      if (len == 0)
      {
        buf = Const::EmptyStr().val.str;
        return 0;
      }

      // TODO: check overflow?
      _stringData[0] = wchar_t(len);
      _stringData[len] = L'\0';
      new (at(i, j)) ExcelObj(PString<wchar_t>(_stringData));
      _stringData += len + 2;
      assert(_stringData <= _endStringData);
      return 0;
    }

    ExcelObj* at(size_t i, size_t j)
    {
      assert(i < _nRows && j < _nColumns);
      return _arrayData + (i * _nColumns + j);
    }

    ExcelObj toExcelObj()
    {
      return ExcelObj(_arrayData, int(_nRows), int(_nColumns));
    }

  private:
    ExcelObj * _arrayData;
    wchar_t* _stringData;
    const wchar_t* _endStringData;
    size_t _nRows, _nColumns;
  };
}