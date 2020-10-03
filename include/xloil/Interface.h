#pragma once
#include "ExportMacro.h"
#include "Register.h"
#include "FuncSpec.h"
#include <memory>
#include <map>

namespace toml { class table; }
namespace xloil { class RegisteredFunc; class AddinContext; }

namespace xloil
{
 
  /// <summary>
  /// A file source collects Excel UDFs created from a single file.
  /// The file could be a plugin DLL or source file. You can inherit
  /// from this class to provide additional tracking functionality.
  /// 
  /// Plugins should avoid keeping references to file sources, or if
  /// they do be careful to clean them up when an XLL detaches
  /// </summary>
  class XLOIL_EXPORT FileSource : public std::enable_shared_from_this<FileSource>
  {
  public:
    /// <summary>
    /// 
    /// </summary>
    /// <param name="sourcePath">Should be a full pathname</param>
    /// <param name="watchFile">Currently unimplemented</param>
    FileSource(
      const wchar_t* sourcePath, 
      const wchar_t* linkedWorkbook=nullptr,
      bool watchFile=false);

    virtual ~FileSource();

    /// <summary>
    /// Registers the given function specifcations with Excel. If
    /// registration fails the input parameter will contain the 
    /// failed functions, otherwise it will be empty. 
    /// 
    /// If this function is called a second time it replaces 
    /// all currently registered functions with the new set.
    /// 
    /// </summary>
    /// <param name="specs">functions to register</param>
    void 
      registerFuncs(
        const std::vector<std::shared_ptr<const FuncSpec> >& specs);

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

    /// <summary>
    /// Removes the specified source from all add-in contexts
    /// </summary>
    /// <param name="context"></param>
    static void
      deleteFileContext(const std::shared_ptr<FileSource>& context);

    const std::wstring& sourcePath() const { return _sourcePath; }
    const std::wstring& linkedWorkbook() const { return _workbookName; }
    const wchar_t* sourceName() const { return _sourceName; }

  private:
    std::map<std::wstring, std::shared_ptr<RegisteredFunc>> _functions;
    std::wstring _sourcePath;
    const wchar_t* _sourceName;
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

    AddinContext(
      const wchar_t* pathName, 
      std::shared_ptr<const toml::table> settings);

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
      auto found = FileSource::findFileContext(sourcePath).first;
      if (found)
      {
        addSource(found);
        return std::make_pair(std::static_pointer_cast<TSource>(found), false);
      }
      else
      {
        auto newSource = std::make_shared<TSource>(std::forward<Args>(args)...);
        addSource(newSource);
        return std::make_pair(newSource, true);
      }
    }

    /// <summary>
    /// Gets the root of the addin's ini file
    /// </summary>
    const toml::table* settings() const { return _settings.get(); }

    /// <summary>
    /// Returns a map of all FileSource associated with this XLL addin
    /// </summary>
    const ContextMap& files() const { return _files; }

    /// <summary>
    /// Returns the full pathname of the XLL addin
    /// </summary>
    const std::wstring& pathName() const { return _pathName; }

    /// <summary>
    /// Returns the filename of the XLL addin
    /// </summary>
    const wchar_t* fileName() const 
    {
      auto slash = _pathName.find_last_of(L'\\');
      return _pathName.c_str() + slash + 1;
    }

    void addSource(const std::shared_ptr<FileSource>& source)
    {
      _files.emplace(std::make_pair(source->sourcePath(), source));
    }

    void removeSource(ContextMap::const_iterator which);

  private:
    AddinContext(const AddinContext&) = delete;
    AddinContext& operator=(const AddinContext&) = delete;

    std::wstring _pathName;
    std::shared_ptr<const toml::table> _settings;
    ContextMap _files;
  };

/// <summary>
/// This macro declares the plugin entry point. Its arguments must match
/// <see cref="PluginInitFunc"/>.
/// </summary>
#define XLO_PLUGIN_INIT(...) extern "C" __declspec(dllexport) int \
  XLO_PLUGIN_INIT_FUNC##(__VA_ARGS__) noexcept

#define XLO_PLUGIN_INIT_FUNC xloil_init

  /// <summary>
  /// Contains information the plugin can use to initialise or 
  /// link with another addin
  /// </summary>
  struct PluginContext
  {
    enum Action
    {
      /// <summary>
      /// The Load action is specified the first time a plugin is initialised
      /// </summary>
      Load,
      /// <summary>
      /// The Attach action is used when an XLL addin has requested use of the 
      /// plugin. The addin may have a settings file which the plugin should process
      /// </summary>
      Attach,
      /// <summary>
      /// Detach is called when an XLL using the plugin is unloading
      /// </summary>
      Detach,
      /// <summary>
      /// When Unload is called, the plugin should clean up all internal
      /// data in anticipation of a call to FreeLibrary.
      /// </summary>
      Unload
    };
    Action action;
    const wchar_t* pluginName;
    const toml::table* settings;
  };

  /// <summary>
  /// A plugin must declare an extern C function with this signature
  /// </summary>
  typedef int(*PluginInitFunc)(AddinContext*, const PluginContext&);

  /// <summary>
  /// Links a plug-in's *spdlog* instance to the main xlOil log output. 
  /// You don't have to do this if you're organising your own logging.
  /// </summary>
  /// <param name=""></param>
  /// <param name="plugin"></param>
  XLOIL_EXPORT void linkLogger(AddinContext*, const PluginContext& plugin);
}
