#pragma once
#include "ExcelObj.h"
#include "Events.h"
#include "Caller.h"
#include <unordered_map>
#include <string_view>
#include <mutex>

namespace xloil
{
  namespace detail
  {
    inline PString<> writeCacheId(const CallerInfo& caller, uint16_t padding)
    {
      PString<> pascalStr(caller.addressRCLength() + padding + 1);
      auto* buf = pascalStr.pstr() + 1;

      wchar_t nWritten = 1; // Leave space for uniquifier

      // Full cell address: eg. [wbName]wsName!RxCy
      nWritten += (wchar_t)caller.writeAddress(buf, pascalStr.length() - 1, false);

      // Fix up length
      pascalStr.resize(nWritten + padding);

      return pascalStr;
    }

    inline PString<> writeCacheId(const wchar_t* caller, uint16_t padding)
    {
      const auto lenCaller = std::min<wchar_t>(
        wcslen(caller), UINT16_MAX - padding - 1);

      PString<> pascalStr(lenCaller + padding + 1);
      auto* buf = pascalStr.pstr() + 1;

      wchar_t nWritten = 1; // Leave space for uniquifier

      // Full cell address: eg. [wbName]wsName!RxCy
      nWritten += lenCaller;
      wcscpy_s(buf, pascalStr.length() - 1, caller);

      // Fix up length
      pascalStr.resize(nWritten + padding);

      return pascalStr;
    }

    // We need to explicitly define our own hash and compare so we can lookup
    // string_view objects without first writing them to string. If that sounds
    // like premature optimisation, it's because it is!
    template<class Val>
    struct Lookup : public std::unordered_map<
      std::wstring,
      std::shared_ptr<Val>>
    {
      // I thought this repeating of base template parameters was fixed in C++17
      // ... but what would C++ be without verbosity.
      using const_iterator = typename std::unordered_map<
        std::wstring,
        std::shared_ptr<Val>>::const_iterator;

      template <class T>
      _NODISCARD const_iterator search(const T& _Keyval) const
      {
        size_type _Bucket = std::hash<T>()(_Keyval) & _Mask;
        for (_Unchecked_const_iterator _Where = _Begin(_Bucket); _Where != _End(_Bucket); ++_Where)
          if (_Where->first == _Keyval)
              return _Make_iter(_Where);
        return (end());
      }
    };
  }

  /// <summary>
  /// Creates a dictionary of TObj indexed by cell address.
  /// The cell address used is determined from the currently executing cell
  /// when the "add" method is called.
  /// 
  /// This class is used to allow data which we cannot or do not want to write
  /// to an Excel sheet to be passed between Excel user functions.
  /// 
  /// The cache add a single character uniquifier TUniquifier to returned 
  /// reference strings. This allows very fast rejection of invalid references
  /// during lookup. The uniquifier should be choosen to be unlikely to occur 
  /// at the start of a string before a '['.
  /// 
  /// Example
  /// -------
  /// <code>
  /// static ObjectCache<PyObject*>, L'&', false> theCache
  ///     = ObjectCache<PyObject*>, L'&', false > ();
  /// 
  /// ExcelObj refStr = theCache.add(new PyObject());
  /// PyObject* obj = theCache.fetch(refStr);
  /// </code>
  /// </summary>
  template<class TObj, wchar_t TUniquifier, bool TReverseLookup = false>
  class ObjectCache
  {
  private:
    typedef ObjectCache<TObj, TUniquifier, TReverseLookup> self;
    class CellCache
    {
    private:
      size_t _calcId;
      std::vector<TObj> objects;

    public:
      CellCache() : _calcId(0) {}

      bool getStaleObjects(size_t calcId, std::vector<TObj>& stale)
      {
        if (_calcId != calcId)
        {
          _calcId = calcId;
          objects.swap(stale);
          return true;
        }
        return false;
      }

      size_t add(TObj&& obj)
      {
        objects.emplace_back(std::forward<TObj>(obj));
        return objects.size() - 1;
      }

      bool tryFetch(size_t i, TObj& obj)
      {
        if (i >= objects.size())
          return false;
        obj = objects[i];
        return true;
      }

    };

  private:

 
    using WorkbookCache = detail::Lookup<CellCache>;

    detail::Lookup<WorkbookCache> _cache;
    mutable std::mutex _cacheLock;

    size_t _calcId;

    typename std::conditional<TReverseLookup, 
      std::unordered_map<TObj, std::wstring>,
      char>::type _reverseLookup;
    typename std::conditional<TReverseLookup,
      std::mutex, 
      char>::type _reverseLookupLock;

    std::shared_ptr<const void> _calcEndHandler;
    std::shared_ptr<const void> _workbookCloseHandler;

    void expireObjects()
    {
      // Called by Excel event so will always be synchonised
      ++_calcId; // Wraps at MAX_UINT- doesn't matter
    }

    size_t addToCell(TObj&& obj, CellCache& cacheVal, std::vector<TObj>& staleObjects)
    {
      cacheVal.getStaleObjects(_calcId, staleObjects);
      return cacheVal.add(std::forward<TObj>(obj));
    }

    template<class V> V& findOrAdd(detail::Lookup<V>& m, const std::wstring_view& key)
    {
      auto found = m.search(key);
      if (found == m.end())
      {
        auto p = std::make_shared<V>();
        m.insert(std::make_pair(std::wstring(key), p));
        return *p;
      }
      return *found->second;
    }

    template<class V> V* find(
      detail::Lookup<V>& m, 
      const std::wstring_view& key)
    {
      auto found = m.search(key);
      if (found == m.end())
        return nullptr;
      return found->second.get();
    }

  public:
    ObjectCache()
      : _calcId(1)
    {
      using namespace std::placeholders;

      _calcEndHandler = std::static_pointer_cast<const void>(
        xloil::Event::AfterCalculate().bind(std::bind(std::mem_fn(&self::expireObjects), this)));
      
      _workbookCloseHandler = std::static_pointer_cast<const void>(
        xloil::Event::WorkbookAfterClose().bind(std::bind(std::mem_fn(&self::onWorkbookClose), this, _1)));
    }

    bool fetch(const std::wstring_view& cacheString, TObj& obj)
    {
      if (cacheString[0] != TUniquifier || cacheString[1] != L'[')
        return false;

      constexpr auto npos = std::wstring_view::npos;

      const auto firstBracket = 1;
      const auto lastBracket = cacheString.find_last_of(']');
      if (lastBracket == npos)
        return false;
      const auto comma = cacheString.find_first_of(',', lastBracket);

      auto workbook = cacheString.substr(firstBracket + 1, lastBracket - firstBracket - 1);
      auto sheetRef = cacheString.substr(lastBracket + 1,
        comma == npos ? npos : comma - lastBracket - 1);

      int iResult = 0;
      if (comma != npos)
      {
        // Oh dear, there's no std::from_chars for wchar_t
        wchar_t tmp[4];
        wcsncpy_s(tmp, 4, cacheString.data() + comma + 1, cacheString.length() - comma - 1);
        iResult = _wtoi(tmp);
      }

      {
        std::scoped_lock lock(_cacheLock);

        auto* wbCache = find(_cache, workbook);
        if (!wbCache)
          return false;

        auto* cellCache = find(*wbCache, sheetRef);
        if (!cellCache)
          return false;

        return cellCache->tryFetch(iResult, obj);
      }
    }

    ExcelObj add(TObj&& obj, const wchar_t* caller = nullptr)
    {
      CallerInfo callerInfo;
      constexpr uint8_t padding = 5;

      auto key = caller
        ? detail::writeCacheId(caller, padding)
        : detail::writeCacheId(callerInfo, padding);

      key[0] = TUniquifier;

      // Capture workbook name. pascalStr should have X[wbName]wsName!cellRef.
      // Search backwards because wbName may contain ']'
      auto lastBracket = key.rchr(L']');
      if (lastBracket == PString<>::npos)
        XLO_THROW("ObjectCache::add: caller must be worksheet address");
      auto wbName = std::wstring_view(key.pstr() + 2, lastBracket - 2);

      // Capture sheet ref, i.e. wsName!cellRef
      // Can use wcslen here because of the null padding
      auto wsRef = std::wstring_view(key.pstr() + lastBracket + 1,
        key.length() - padding - lastBracket - 1);

      std::vector<TObj> staleObjects;
      uint8_t iPos = 0;
      {
        std::scoped_lock lock(_cacheLock);

        auto& cellCache = fetchOrAddCell(wbName, wsRef);
        iPos = (uint8_t)addToCell(std::forward<TObj>(obj), cellCache, staleObjects);
      }

      uint8_t nPaddingUsed = 0;
      // Add cell object count in form ",XXX"
      if (iPos > 0)
      {
        auto buf = const_cast<wchar_t*>(key.end()) - padding;
        *(buf++) = L',';
        _itow_s(iPos, buf, padding - 1, 10);
        nPaddingUsed = 1 + (uint8_t)wcsnlen(buf, padding - 1);
      }
        
      key.resize(key.length() - padding + nPaddingUsed);

      if constexpr (TReverseLookup)
      {
        std::scoped_lock lock(_reverseLookupLock);
        for (auto& x : staleObjects)
          _reverseLookup.erase(x);
        _reverseLookup.insert(std::make_pair(obj, key.string()));
      }

      return ExcelObj(std::move(key));
    }

  private:
    CellCache& fetchOrAddCell(const std::wstring_view& wbName, const std::wstring_view& wsRef)
    {
      auto& wbCache = findOrAdd(_cache, wbName);
      return findOrAdd(wbCache, wsRef);
    }

    void onWorkbookClose(const wchar_t* wbName)
    {
      // Called by Excel Event so will always be synchonised
      if constexpr (TReverseLookup)
      {
        auto found = _cache.find(wbName);
        if (found != _cache.end())
        {
          for (auto& cell : *found->second)
            for (auto& obj : cell.second->objects)
              _reverseLookup.erase(obj);
        }
      }
      _cache.erase(wbName);
    }
  };
}