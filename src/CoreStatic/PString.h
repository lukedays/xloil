#pragma once
#include <xlOil/Throw.h>
#include <string>

namespace xloil
{
  template <class TChar = wchar_t>
  class PStringImpl
  {
  public:
    using size_type = TChar;
    static constexpr size_type npos = size_type(-1);


    /// <summary>
    /// Returns true if the string is empty
    /// </summary>
    bool empty() const { return !_data || _data[0] == 0; }

    /// <summary>
    /// Returns the length of the string. The length is limited by
    /// sizeof(TChar).
    /// </summary>
    size_type length() const { return _data ? _data[0] : 0; }
    
    /// <summary>
    /// Returns a pointer to the start of the string data. The string
    /// data is not guaranteed to be null-terminated.
    /// </summary>
    const TChar* pstr() const { return _data + 1; }
    TChar* pstr() { return _data + 1; }

    /// <summary>
    /// Returns a pointer to the raw pascal string buffer.
    /// </summary>
    TChar* data() { return _data; }

    /// <summary>
    /// Returns an iterator (really a pointer) to the beginning of the 
    /// string data.
    /// </summary>
    const TChar* begin() const { return _data + 1; }

    /// <summary>
    /// Returns an iterator (really a pointer) to the end of the 
    /// string data (just past the last character).
    /// </summary>
    const TChar* end() const { return _data + 1 + length(); }

    /// <summary>
    /// Copy the contents of another Pascal string into this one. Throws
    /// if the existing buffer is too short.
    /// </summary>
    PStringImpl operator=(const PStringImpl& that)
    {
      if (this == &that)
        return *this;
      if(!write(that.pstr(), that.length()))
        XLO_THROW("PString buffer too short: {0} required, {1} available", (int)that.length(), (int)length());
      return *this;
    }

    /// <summary>
    /// Writes the given null-terminated string into the buffer, raising an error
    /// if the buffer is too short.
    /// </summary>
    PStringImpl operator=(const TChar* str)
    {
      if(!write(str))
        XLO_THROW("PString buffer too short: {0} required, {1} available", wcslen(str), (int)length());
      return *this;
    }

    /// <summary>
    /// Writes len chars from given string into the buffer, returning true
    /// if successful and false if the internal buffer is too short.
    /// If len is omitted, writes all characters in str up to (but not 
    /// including) the null terminator.
    /// </summary>
    bool write(const TChar* str, int len = -1)
    {
      if (len < 0)
        len = (int)wcslen(str);
      if (len > length())
        return false;
      if (len > 0)
        wmemcpy_s(_data + 1, _data[0], str, len);
      _data[0] = len;
      return true;
    }

    /// <summary>
    /// Returns an STL string representation of the pascal string. This
    /// copies the string data.
    /// </summary>
    std::basic_string<TChar> string() const 
    { 
      return std::basic_string<TChar>(pstr(), pstr() + length()); 
    }

    /// <summary>
    /// Searches backward for the specified char returning a pointer
    /// to its last occurence or null if not found. Essentialy it is
    /// wmemchr backwards.
    /// </summary>
    static const TChar* wmemrchr(const TChar* ptr, TChar wc, size_type num)
    {
      for (; num; --ptr, --num)
        if (*ptr == wc)
          return ptr;
      return nullptr;
    }

    /// <summary>
    /// Searches forward for the specified char returning its the offset
    /// of its first occurence or npos if not found.
    /// </summary>
    size_type chr(TChar needle) const
    {
      auto p = wmemchr(pstr(), needle, length());
      return p ? p - pstr() : npos;
    }

    /// <summary>
    /// Searches backward for the specified char returning its the offset
    /// of its last occurence or npos if not found.
    /// </summary>
    size_type rchr(TChar needle) const
    {
      auto p = wmemrchr(pstr() + length(), needle, length());
      return p ? (size_type)(p - pstr()) : npos;
    }

    /// <summary>
    /// Returns a STL string_view of the string data or, optionally,
    /// a substring of it.
    /// </summary>
    std::basic_string_view<TChar> view(size_type from = 0, size_type count = npos)
    {
      return std::basic_string_view<TChar>(pstr() + from, count != npos ? count : length() - from);
    }

  protected:
    TChar* _data;

    PStringImpl(TChar* data)
      : _data(data)
    {}
  };


  /// <summary>
  /// A Pascal string is a length-counted, rather than null-terminated string
  /// The first character in the string buffer contains its length, and the 
  /// remaining characters contain the content. This type of string is used in
  /// by Excel in its xloper (ExcelObj type in xlOil). PString helps to handle
  /// these Pascal strings by behaving somewhat like std::string, however it 
  /// is not recommended to use this type for generic string manipuation.
  ///  
  /// PString owns its data buffer, <seealso cref="PStringView"/> does not.
  /// </summary>
  template <class TChar = wchar_t>
  class PString : public PStringImpl<TChar>
  {
  public:
    using size_type = PStringImpl::size_type;

    /// <summary>
    /// Create a PString of the specified length
    /// </summary>
    PString(size_type length)
      : PStringImpl(new TChar[length + 1])
    {
      resize(length);
    }

    /// <summary>
    /// Take ownership of a Pascal string buffer, constructed externally 
    /// </summary>
    PString(TChar* data)
      : PStringImpl(data)
    {}

    ~PString()
    {
      delete[] _data;
    }

    /// <summary>
    /// Returns a pointer to the buffer containing the string and
    /// releases ownership.
    /// </summary>
    TChar* release()
    {
      TChar* d = nullptr;
      std::swap(d, _data);
      return d;
    }

    /// <summary>
    /// Resize the string buffer to the specified length. Increasing
    /// the length forces a string copy.
    /// </summary>
    void resize(size_type sz)
    { 
      if (sz <= length())
        _data[0] = sz;
      else
      {
        PString copy(sz);
        copy = *this;
        copy._data[0] = sz;
        std::swap(copy._data, _data);
      }
    }
  };

  /// <summary>
  /// A view i.e. non-owning, a Pascal string, see the discussion in 
  /// <seealso cref="PString"/> for background on Pascal strings
  /// 
  /// This class cannot view  a sub-string of a Pascal string, but 
  /// calling the <see cref="PStringView::view"/> method returns
  /// a std::string_view class which can.
  /// </summary>
  template <class TChar=wchar_t>
  class PStringView : public PStringImpl<TChar>
  {
  public:
    /// <summary>
    /// Constructs a view of an existing Pascal string given its
    /// full data buffer (including the length count).
    /// </summary>
    PStringView(TChar* data = nullptr)
      : PStringImpl(data)
    {}
  };
}