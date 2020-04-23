#pragma once
#include "ExportMacro.h"
#include "Register.h"
#include "ExcelObj.h"
#include "ExcelObjCache.h"
#include "FuncSpec.h"
#include <memory>
#include <map>

namespace toml {
  template<typename, template<typename...> class, template<typename...> class> class basic_value;
  struct discard_comments;
  using value = basic_value<discard_comments, std::unordered_map, std::vector>;
}
namespace Excel { struct _Application; }
namespace xloil { class RegisteredFunc; }
namespace spdlog { class logger; }

namespace xloil
{
  namespace Core
  {
    /// <summary>
    /// Returns the full path to the xloil Core dll, including the filename
    /// </summary>
    /// <returns></returns>
    XLOIL_EXPORT const wchar_t* theCorePath();

    /// <summary>
    /// Returns just the filename of the xloil Core dll
    /// </summary>
    /// <returns></returns>
    XLOIL_EXPORT const wchar_t* theCoreName();

    XLOIL_EXPORT int theExcelVersion();

    /// <summary>
    /// Give a reference to the COM Excel application object for the
    /// running instance
    /// </summary>
    XLOIL_EXPORT Excel::_Application& theExcelApp();

    /// <summary>
    /// Returns true if the function wizard dialogue box is being used.
    /// Quite an expensive check.
    /// </summary>
    XLOIL_EXPORT bool inFunctionWizard();

    /// <summary>
    /// Throws '#WIZARD!' if the function wizard dialogue box is being used.
    /// Quite an expensive check.
    /// </summary>
    XLOIL_EXPORT void throwInFunctionWizard();

    /// <summary>
    /// Returns true if the provided string contains the magic chars
    /// for the ExcelObj cache. Expects a counted string.
    /// </summary>
    /// <param name="str">Pointer to string start</param>
    /// <param name="length">Number of chars to read</param>
    /// <returns></returns>
    inline bool
      maybeCacheReference(const wchar_t* str, size_t length)
    {
      return checkObjectCacheReference(str, length);
    }

    XLOIL_EXPORT bool
      fetchCache(
        const wchar_t* cacheString, 
        size_t length, 
        std::shared_ptr<const ExcelObj>& obj);

    XLOIL_EXPORT ExcelObj
      insertCache(std::shared_ptr<const ExcelObj>&& obj);

    inline ExcelObj
      insertCache(ExcelObj&& obj)
    {
      return insertCache(std::make_shared<const ExcelObj>(std::forward<ExcelObj>(obj)));
    }
  }

  /// <summary>
  /// A file source collects Excel UDFs created from a single file.
  /// The file could be a plugin DLL or source file. You can inherit
  /// from this class to provide additional tracking functionality.
  /// 
  /// Plugins should avoid keeping references to file sources, or if
  /// they do be careful to clean them up when an XLL detaches
  /// </summary>
  class XLOIL_EXPORT FileSource
  {
  public:
    /// <summary>
    /// 
    /// </summary>
    /// <param name="sourcePath">Should be a full pathname</param>
    /// <param name="watchFile">currently unimplemented</param>
    FileSource(const wchar_t* sourceName, bool watchFile=false);

    virtual ~FileSource();

    /// <summary>
    /// Registers the given function specifcations with Excel. If
    /// registration fails the input parameter will contain the 
    /// failed functions, otherwise it will be empty. 
    /// 
    /// If this function is called a second time it replaces 
    /// all currently registered functions with the new set.
    /// 
    /// Returns true if all function registrations suceeded.
    /// </summary>
    /// <param name="specs">functions to register</param>
    bool 
      registerFuncs(
        std::vector<std::shared_ptr<const FuncSpec> >& specs);

    /// <summary>
    /// Removes the specified function from Excel
    /// </summary>
    /// <param name="name"></param>
    /// <returns></returns>
    bool
      deregister(const std::wstring& name);
    
    /// <summary>
    /// Registers the given functions as local functions in the specified
    /// workbook
    /// </summary>
    /// <param name="workbookName"></param>
    /// <param name="funcInfo"></param>
    /// <param name="funcs"></param>
    void 
      registerLocal(
        const wchar_t* workbookName,
        const std::vector<std::shared_ptr<const FuncInfo>>& funcInfo,
        const std::vector<ExcelFuncObject> funcs);

    /// <summary>
    /// Looks for a FileSource corresponding the specified pathname.
    /// Returns the FileSource if found, otherwise a null pointer
    /// </summary>
    /// <param name="sourcePath"></param>
    /// <returns></returns>
    static std::pair<std::shared_ptr<FileSource>, std::shared_ptr<AddinContext>>
      findFileContext(const wchar_t* sourcePath);

    static void
      deleteFileContext(const std::shared_ptr<FileSource>& context);

    const std::wstring& sourceName() const { return _source; }

  private:
    std::map<std::wstring, std::shared_ptr<RegisteredFunc>> _functions;
    std::wstring _source;
    std::wstring _workbookName;

    // TODO: implement std::string _functionPrefix;

    std::shared_ptr<RegisteredFunc> registerFunc(
      const std::shared_ptr<const FuncSpec>& spec);
  };

  /// <summary>
  /// The AddinContext keeps track of file sources associated with an Addin
  /// to ensure they are properly cleaned up when the addin unloads
  /// </summary>
  class AddinContext
  {
  public:
    using ContextMap = std::map<std::wstring, std::shared_ptr<FileSource>>;

    AddinContext(const wchar_t* pathName, std::shared_ptr<const toml::value> settings);
    ~AddinContext();

    /// <summary>
    /// Links a FileSource for the specified source path to this
    /// add-in context. Other addin contexts are first searched
    /// for the matching FileSource.  If it is not found a new
    /// one is created passing the variadic argument to the TSource
    /// constructor.
    /// </summary>
    template <class TSource, class...Args>
    std::pair<std::shared_ptr<TSource>, bool>
      tryAdd(
        const wchar_t* sourcePath, Args&&... args)
    {
      auto found = FileSource::findFileContext(sourcePath);
      if (found)
      {
        _files[sourcePath] = found;
        return std::make_pair(std::static_pointer_cast<TSource>(found), false);
      }
      else
      {
        auto newSource = std::make_shared<TSource>(std::forward<Args>(args)...);
        _files[sourcePath] = newSource;
        return std::make_pair(newSource, true);
      }
    }

    /// <summary>
    /// Gets the default logger for this add-in. Currently this is 
    /// shared accross all addins - there is only one log file - but
    /// this will change eventually.
    /// </summary>
    XLOIL_EXPORT std::shared_ptr<spdlog::logger> 
      getLogger() const;

    // TODO: XLOIL_EXPORT void log_error(const std::wstring& msg);

    /// <summary>
    /// Gets the root of the addin's ini file
    /// </summary>
    const toml::value* settings() const { return _settings.get(); }

    /// <summary>
    /// Returns a map of all FileSource associated with this XLL addin
    /// </summary>
    const ContextMap& files() const { return _files; }

    /// <summary>
    /// Returns the full pathname of the XLL addin
    /// </summary>
    const std::wstring& pathName() const { return _pathName; }

    void removeFileSource(ContextMap::const_iterator which);

  private:
    AddinContext(const AddinContext&) = delete;
    AddinContext& operator=(const AddinContext&) = delete;

    std::wstring _pathName;
    std::shared_ptr<const toml::value> _settings;
    ContextMap _files;
  };


#define XLO_PLUGIN_INIT(...) extern "C" __declspec(dllexport) int \
  XLO_PLUGIN_INIT_FUNC##(__VA_ARGS__) noexcept

#define XLO_PLUGIN_INIT_FUNC xloil_init

  // TODO: Think of a better name?
  struct PluginContext
  {
    enum Action
    {
      Load,
      Attach,
      Detach,
      Unload
    };
    Action action;
    const wchar_t* pluginName;
    const toml::value* settings;
  };


  typedef int(*PluginInitFunc)(AddinContext*, const PluginContext&);
}
