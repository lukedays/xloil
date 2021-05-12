#pragma once
#include <xlOil/ExcelObj.h>

namespace xloil
{
  /// <summary>
  /// A Range holds refers to part of an Excel sheet. It can use either the
  /// XLL or COM interfaces to interact with Excel. Range can only be used by
  /// macro-enabled functions or event call-backs.
  /// 
  /// Currently only single area ranges are supported
  /// </summary>
  class Range
  {
  public:
    using row_t = ExcelObj::row_t;
    using col_t = ExcelObj::col_t;

    static constexpr int TO_END = INT_MAX;
    virtual ~Range();

    /// <summary>
    /// Gives a subrange relative to the current range.  Similar to Excel's function, 
    /// we do not insist the sub-range is a subset, so fromRow can be negative or toRow 
    /// can be past the end of the referenced range. Unlike Excel, the indices are zero-based
    /// Omitting toRow or fromRow or passing the special value TO_END goes to the end of the 
    /// parent range.
    /// </summary>
    /// <param name="fromRow"></param>
    /// <param name="fromCol"></param>
    /// <param name="toRow"></param>
    /// <param name="toCol"></param>
    /// <returns></returns>
    virtual Range* range(
      int fromRow, int fromCol,
      int toRow = TO_END, int toCol = TO_END) const = 0;

    /// <summary>
    /// Returns a 1x1 subrange containing the specified cell. Uses zero-based
    /// indexing unlike Excel's VBA Range.Cells function.
    /// </summary>
    /// <param name="i"></param>
    /// <param name="j"></param>
    /// <returns></returns>
    Range* cell(int i, int j) const
    {
      return range(i, j, i, j);
    }

    virtual std::tuple<row_t, col_t> shape() const = 0;
    
    virtual std::tuple<row_t, col_t, row_t, col_t> bounds() const = 0;

    /// <summary>
    /// Returns the number of rows in the range
    /// </summary>
    row_t nRows() const
    {
      return std::get<0>(shape());
    }

    /// <summary>
    /// Returns the number of columns in the range
    /// </summary>
    col_t nCols() const
    {
      return std::get<1>(shape());
    }

    size_t size() const
    {
      row_t nRows; col_t nCols;
      std::tie(nRows, nCols) = shape();
      return nRows * nCols;
    }

    /// <summary>
    /// Returns the address of the range in the form
    /// 'SheetNm!A1:Z5'
    /// </summary>
    virtual std::wstring address(bool local = false) const = 0;

    /// <summary>
    /// Converts the referenced range to an ExcelObj. References
    /// to single cells return an ExcelObj of the appropriate type.
    /// Multicell references return an array.
    /// </summary>
    virtual ExcelObj value() const = 0;
    
    /// <summary>
    /// Convenience wrapper for cell(i,j)->value()
    /// </summary>
    virtual ExcelObj value(row_t i, col_t j) const = 0;

    /// <summary>
    /// Convience wrapper for value(i, j). Note writing to the returned value 
    /// does not set values in the range. 
    /// </summary>
    ExcelObj operator()(int i, int j) const
    {
      return value(i, j);
    }

    /// <summary>
    /// Sets the cell values in the range to the provided value. 
    /// If `value` is a single value, every cell will be set to 
    /// that value.
    /// </summary>
    virtual void set(const ExcelObj& value) = 0;

    Range& operator=(const ExcelObj& value)
    {
      set(value);
      return *this;
    }

    /// <summary>
    /// Clears / empties all cells referred to by this ExcelRange.
    /// </summary>
    virtual void clear() = 0;
  };

  XLOIL_EXPORT Range* newXllRange(const ExcelObj& xlRef);

  /// <summary>
  /// Creates an appropriate range type depending on whether the XLL
  /// interface is available.
  /// </summary>
  XLOIL_EXPORT Range* newRange(const wchar_t* address);
}