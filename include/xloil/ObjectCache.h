#pragma once
#include <xloil/ExcelObj.h>
#include <xloil/PString.h>
#include <xloil/Events.h>
#include <xloil/Caller.h>
#include <xloil/Throw.h>
#include <unordered_map>
#include <string_view>
#include <mutex>

namespace xloil
{
  template<class T>
  struct CacheUniquifier
  {
    CacheUniquifier()
    {
      static wchar_t chr = L'\xC38';
      value = chr++;
    }
    wchar_t value;
  };

  namespace detail
  {
    inline auto writeCacheId(const CallerInfo& caller, wchar_t padding)
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

    inline PString<> writeCacheId(const wchar_t* caller, wchar_t padding)
    {
      const auto lenCaller = (wchar_t)std::min<size_t>(
        wcslen(caller), UINT16_MAX - padding - 1);
      PString<> pascalStr(lenCaller + padding + 1);
      pascalStr.replace(1, lenCaller, caller);
      return pascalStr;
    }

    // We need to explicitly define our own hash and compare so we can lookup
    // string_view objects without first writing them to string. If that sounds
    // like premature optimisation, it's because it is!
    template<class Val>
    struct Lookup : public std::unordered_map<
      std::wstring,
      std::unique_ptr<Val>>
    {
      // I thought this repeating of base template parameters was fixed in C++17
      // ... but what would C++ be without verbosity.
      using const_iterator = typename std::unordered_map<
        std::wstring,
        std::unique_ptr<Val>>::const_iterator;

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
  template<class TObj, class TUniquifier, bool TReverseLookup = false>
  class ObjectCache
  {
  private:
    typedef ObjectCache<TObj, TUniquifier, TReverseLookup> self;
    class CellCache
    {
    private:
      size_t _calcId;
      std::vector<TObj> _objects;

    public:
      CellCache() : _calcId(0) {}

      const std::vector<TObj>& objects() const
      {
        return _objects;
      }

      void getStaleObjects(size_t calcId, std::vector<TObj>& stale)
      {
        if (_calcId != calcId)
        {
          _calcId = calcId;
          _objects.swap(stale);
        }
      }

      size_t add(TObj&& obj)
      {
        _objects.emplace_back(std::forward<TObj>(obj));
        return _objects.size() - 1;
      }

      bool tryFetch(size_t i, const TObj*& obj) const
      {
        if (i >= _objects.size())
          return false;
        obj = &_objects[i];
        return true;
      }
    };

  private:
    detail::Lookup<CellCache> _cache;
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
      ++_calcId; // Wraps at MAX_UINT - but this doesn't matter
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
        auto it = m.emplace(std::make_pair(std::wstring(key), std::make_unique<V>()));
        return *it.first->second;
      }
      return *found->second;
    }

    template<class V> V* find(
      detail::Lookup<V>& m, 
      const std::wstring_view& key)
    {
      auto found = m.search(key);
      return found == m.end() 
        ? nullptr 
        : found->second.get();
    }

    static constexpr uint8_t PADDING = 2;

  public:
    TUniquifier _uniquifier;

    ObjectCache(bool reapOnWorkbookClose = true)
      : _calcId(1)
    {
      using namespace std::placeholders;

      _calcEndHandler = std::static_pointer_cast<const void>(
        xloil::Event::AfterCalculate().bind(std::bind(std::mem_fn(&self::expireObjects), this)));
      
      if (reapOnWorkbookClose)
        _workbookCloseHandler = std::static_pointer_cast<const void>(
          xloil::Event::WorkbookAfterClose().bind([this](auto wbName) { this->onWorkbookClose(wbName); }));
    }

    bool fetchIfValid(const std::wstring_view& cacheString, TObj*& obj)
    {
      return !checkValid(cacheString)
        ? false
        : fetch(const std::wstring_view& cacheString, TObj*& obj)
    }

    bool fetch(const std::wstring_view& key, const TObj*& obj)
    {
      const auto iResult = readCount(key[key.size() - 1]);
      const auto cacheKey = key.substr(0, key.size() - PADDING);

      std::scoped_lock lock(_cacheLock);
      const auto* cellCache = find(_cache, cacheKey);
      if (!cellCache)
        return false;

      return cellCache->tryFetch(iResult, obj);
    }

    ExcelObj add(TObj&& obj, const wchar_t* caller = nullptr)
    {
      CallerInfo callerInfo;
      
      auto fullKey = caller
        ? detail::writeCacheId(caller, PADDING)
        : detail::writeCacheId(callerInfo, PADDING);

      fullKey[0] = _uniquifier.value;

      auto cacheKey = fullKey.view(0, fullKey.length() - PADDING);

      std::vector<TObj> staleObjects;
      uint8_t iPos = 0;
      {
        std::scoped_lock lock(_cacheLock);

        auto& cellCache = findOrAdd(_cache, cacheKey);
        iPos = (uint8_t)addToCell(std::forward<TObj>(obj), cellCache, staleObjects);
      }

      writeCount(fullKey.end() - PADDING, iPos);

      if constexpr (TReverseLookup)
      {
        std::scoped_lock lock(_reverseLookupLock);
        for (auto& x : staleObjects)
          _reverseLookup.erase(x);
        _reverseLookup.insert(std::make_pair(obj, fullKey.string()));
      }

      return ExcelObj(std::move(fullKey));
    }

    /// <summary>
    /// Remove the given cache reference and any associated objects
    /// This should only be called with manually specifed cache reference
    /// strings. Note the counter (,NNN) after the cache reference is ignored
    /// if specifed and all matching objects are removed.
    /// </summary>
    /// <param name="cacheRef">cache reference to remove</param>
    /// <returns>true if removal succeeded, otherwise false</returns>
    bool remove(const std::wstring_view& key)
    {
      auto cacheKey = key.substr(0, key.length() - PADDING);

      std::scoped_lock lock(_cacheLock);
      auto found = _cache.search(cacheKey);
      if (found == _cache.end())
        return false;
      _cache.erase(found);
      return true;
    }

    void onWorkbookClose(const wchar_t* wbName)
    {
      // Called by Excel Event so will always be synchonised
      const auto len = wcslen(wbName);
      auto i = _cache.begin();
      while (i != _cache.end()) {
        // Key looks like [WbName]BlahBlah - check for match
        if (wcsncmp(wbName, i->first.c_str() + 1, len) == 0)
        {
          if constexpr (TReverseLookup)
          {
            for (auto& obj : i->second->objects)
              _reverseLookup.erase(obj);
          }
          i = _cache.erase(i);
        }
        else
          ++i;
      }
    }

    auto begin() const
    {
      return _cache.cbegin();
    }

    auto end() const
    {
      return _cache.cend();
    }

    std::wstring writeKey(
      const std::wstring_view& cacheKey,
      size_t count) const
    {
      const auto keyLen = cacheKey.length();
      std::wstring key;
      key.resize(keyLen + PADDING);
      key.replace(0, keyLen, cacheKey);
      writeCount(key.data() + keyLen, count);
      return key;
    }

    bool checkValid(const std::wstring_view& cacheString)
    {
      return cacheString.size() > 4
        && cacheString[0] == _uniquifier.value
        && cacheString[1] == L'['
        && cacheString[cacheString.length() - PADDING] == L',';
    }

  private:

    size_t readCount(wchar_t count) const
    {
      return (size_t)(count - 65);
    }

    /// Add cell object count in form ",X"
    void writeCount(wchar_t* key, size_t iPos) const
    {
      key[0] = L',';
      // An offset of 65 means we start with 'A'
      key[1] = (wchar_t)(iPos + 65);
    }
  };

  template<typename T>
  struct ObjectCacheFactory
  {
    static auto& cache() {
      static ObjectCache<T, CacheUniquifier<T>, false> theInstance;
      return theInstance;
    }
  };

  template<typename T, typename... Args>
  inline auto make_cached(Args&&... args)
  {
    return ObjectCacheFactory<std::unique_ptr<const T>>::cache().add(
      std::make_unique<T>(std::forward<Args>(args)...));
  }
  template<typename T>
  inline auto make_cached(const T* ptr)
  {
    return ObjectCacheFactory<std::unique_ptr<const T>>::cache().add(
      std::unique_ptr<const T>(ptr));
  }

  template<typename T>
  inline const T* get_cached(const std::wstring_view& key)
  {
    if (!ObjectCacheFactory<std::unique_ptr<const T>>::cache().checkValid(key))
      return nullptr;

    const std::unique_ptr<const T>* obj = nullptr;
    auto ret = ObjectCacheFactory<std::unique_ptr<const T>>::cache().fetch(key, obj);
    return ret ? obj->get() : nullptr;
  }
}