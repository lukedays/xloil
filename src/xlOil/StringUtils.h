#pragma once
#include <string>
#include <codecvt>

#define XLO_STR(s) XLO_STR_IMPL(s)
#define XLO_STR_IMPL(s) #s

namespace xloil
{
  inline std::string utf16ToUtf8(const std::wstring_view& str)
  {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(str.data());
  }

  inline std::wstring utf8ToUtf16(const std::string_view& str)
  {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.from_bytes(str.data());
  }


  // http://unicode.org/faq/utf_bom.html
  constexpr char32_t LEAD_OFFSET = (char32_t)(0xD800 - (0x10000 >> 10));
  constexpr char32_t SURROGATE_OFFSET = (char32_t)(0x10000 - (0xD800 << 10) - 0xDC00);
  constexpr char32_t HI_SURROGATE_START = 0xD800;

  struct ConvertUTF16ToUTF32
  {
    using to_char = char32_t;
    using from_char = char16_t;

    size_t operator()(
      to_char* target, 
      const size_t size, 
      const from_char* begin, 
      const from_char* end) const
    {
      auto* p = target;
      auto* pEnd = target + size;
      for (; begin < end; ++begin, ++p)
      {
        // If we are past the end of the buffer, carry on so we can
        // determine the required buffer length, but do not write
        // any characters
        if (p == pEnd)
        {
          if (*begin >= HI_SURROGATE_START) 
            ++begin;
        }
        else
        {
          if (*begin < HI_SURROGATE_START)
            *p = *begin;
          else
          {
            auto lead = *begin++;
            *p = (lead << 10) + *begin + SURROGATE_OFFSET;
          }
        }
      }
      return p - target;
    }
    size_t operator()(
      to_char* target, 
      const size_t size, 
      const wchar_t* begin, 
      const wchar_t* end) const
    {
      return (*this)(target, size, (const from_char*)begin, (const from_char*)end);
    }
  };

  struct ConvertUTF32ToUTF16
  {
    using from_char = char32_t;
    using to_char = char16_t;
    static void convertChar(char32_t codepoint, char16_t &h, char16_t &l)
    {
      if (codepoint < 0x10000)
      {
        h = (char16_t)codepoint;
        l = 0;
        return;
      }
      h = (char16_t)(LEAD_OFFSET + (codepoint >> 10));
      l = (char16_t)(0xDC00 + (codepoint & 0x3FF));

    }
    size_t operator()(
      to_char* target, 
      const size_t size, 
      const from_char* begin, 
      const from_char* end) const
    {
      auto* p = target;
      auto* pEnd = target + size;
      to_char lead, trail;
      for (; begin != end; ++begin, ++p)
      {
        convertChar(*begin, lead, trail);
        // If we are past the end of the buffer, carry on so we can
        // determine the required buffer length, but do not write
        // any characters
        if (p + 1 >= pEnd)
        {
          if (trail != 0) 
            ++p;
        }
        else
        {
          *p = lead;
          if (trail != 0)
            *(++p) = trail;
        }
      }
      return p - target;
    }
    size_t operator()(
      wchar_t* target, 
      const size_t size,
      const from_char* begin, 
      const from_char* end) const
    {
      return (*this)((to_char*)target, size, begin, end);
    }
  };

  template <class TInt> inline
  bool floatingToInt(double d, TInt& i)
  {
    double intpart;
    if (std::modf(d, &intpart) != 0.0)
      return false;

    // todo: ? std::numeric_limits<TInt>::
    if (!(intpart > INT_MIN && intpart < INT_MAX))
      return false;

    i = int(intpart);
    return true;
  }

  /// <summary>
  /// Helper function to capture C++ strings from Windows Api functions which have signatures like
  ///    int_charsWritten GetTheString(wchar* buffer, int bufferSize);
  /// </summary>
  template<class F>
  std::wstring captureStringBuffer(F bufWriter, size_t initialSize = 1024)
  {
    std::wstring s;
    s.reserve(initialSize);
    size_t len;
    // We assume, hopefully correctly, that the bufWriter function on
    // failure returns either -1 or the required buffer length.
    while ((len = bufWriter(s.data(), s.capacity())) > s.capacity())
      s.reserve(len == size_t(-1) ? s.size() * 2 : len);
    
    s._Eos(len);
    s.shrink_to_fit();
    return s;
  }

  std::wstring getEnvVar(const wchar_t* name);

  /// <summary>
  /// Sets an environment variable, unsets when the object goes out of scope.
  /// </summary>
  class PushEnvVar
  {
  private:
    std::wstring _previous;
    std::wstring _name;

  public:
    PushEnvVar(const std::wstring& name, const std::wstring& value);
    PushEnvVar(const wchar_t* name, const wchar_t* value);
    ~PushEnvVar();
    void pop();
  };
}