#pragma once
#include <string>

namespace xloil
{
  template <class TChar = wchar_t>
  class PString
  {
  public:
    using size_type = TChar;
    static constexpr size_type npos = size_type(-1);

    PString(size_type size)
      : _data(new TChar[size + 1])
      , _owner(true)
    {
      resize(size);
    }

    PString() : _data(nullptr), _owner(false) 
    {}

    PString(TChar*&& str)
      : _data(str)
      , _owner(true)
    {}

    ~PString()
    {
      if (_owner)
        delete[] _data;
    }

    static PString<> view(TChar* str)
    {
      return PString(str, false);
    }
    static PString<> own(TChar* str)
    {
      return PString(str, true);
    }

    bool operator!() const { return !!_data; }
    size_type length() const { return _data ? _data[0] : 0; }
    void resize(size_type s) { _data[0] = s; }
    
    const TChar* pstr() const { return _data + 1; }
    TChar* pstr() { return _data + 1; }
    const TChar* begin() const { return _data + 1; }
    const TChar* end() const { return _data + 1 + length(); }

    TChar* release() 
    { 
      if (_owner)
      {
        _owner = false;
        TChar* d = nullptr;
        std::swap(d, _data);
        return d;
      }
      return _data;
    }

    PString operator=(const TChar* str)
    {
      auto len = wcslen(str);
      if (len > length())
        XLO_THROW("PString buffer too short");
      wmemcpy_s(_data + 1, _data[0], str, len);
      _data[0] = len;
    }
    std::basic_string<TChar> string() const 
    { 
      return std::basic_string<TChar>(pstr(), pstr() + length()); 
    }

    // Like wmemchr but backwards!
    static const TChar* wmemrchr(const TChar* ptr, TChar wc, size_type num)
    {
      for (; num; --ptr, --num)
        if (*ptr == wc)
          return ptr;
      return nullptr;
    }

    size_type chr(TChar wc) const
    {
      auto p = wmemchr(pstr(), wc, length());
      return p ? p - pstr() : npos;
    }

    size_type rchr(TChar wc) const
    {
      auto p = wmemrchr(pstr() + length(), wc, length());
      return p ? (size_type)(p - pstr()) : npos;
    }

    std::basic_string_view<TChar> view(size_type from, size_type count = npos)
    {
      return std::basic_string_view<TChar>(pstr() + from, count != npos ? count : length() - from);
    }

  private:
    TChar* _data;
    bool _owner;

    PString(TChar* pascalStr, bool owner)
      : _data(pascalStr)
      , _owner(owner)
    {}
  };

}