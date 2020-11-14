#include <xlOil/ExcelObjCache.h>
#include <xlOil/ObjectCache.h>
#include "PyCoreModule.h"
#include "BasicTypes.h"
#include "Cache.h"
#include "Main.h"
namespace py = pybind11;
using std::wstring;

namespace xloil 
{
  template<>
  struct CacheUniquifier<py::object>
  {
    static constexpr wchar_t value = L'\x6B23';
  };
  using pyCacheUnquifier = CacheUniquifier<py::object>;

  namespace Python {

    namespace
    {
      struct PyCache;
      static PyCache* thePythonObjCache = nullptr;

      // Only a single instance of this class is created
      struct PyCache
      {
        PyCache()
          : _cache(false)
        {
          thePythonObjCache = this;
          _workbookCloseHandler = std::static_pointer_cast<const void>(
            xloil::Event::WorkbookAfterClose().bind(
              [this](auto wbName)
              { 
                py::gil_scoped_acquire getGil;
                _cache.onWorkbookClose(wbName); 
              }));
        }

        // Just to prevent any potential errors!
        PyCache(const PyCache& that) = delete;

        ~PyCache()
        {
          XLO_TRACE("Python object cache destroyed");
        }

        py::object add(const py::object& obj, const wchar_t* tag=nullptr)
        {
          // The cache expects callers to be of the form [.]xxx, so we add
          // a prefix if a custom tag is specified. Note the forward slash
          // cannot appear in a workbook name so this tag never collides with
          // the caller-based default
          const auto cacheKey = _cache.add(py::object(obj), tag 
              ? CallerInfo(ExcelObj(tag))
              : CallerInfo());
          return PySteal(detail::PyFromString()(cacheKey.asPString()));
        }
        py::object get(const std::wstring_view& str)
        {
          const ExcelObj* xlObj = getCached<ExcelObj>(str);
          if (xlObj)
            return PySteal(PyFromAny()(*xlObj));

          auto* obj = _cache.fetch(str);
          return obj ? *obj : py::none(); // TODO: More pythonic to throw?
        }
        bool remove(const std::wstring& cacheRef)
        {
          return _cache.erase(cacheRef);
        }
        bool contains(const std::wstring_view& str)
        {
          return _cache.fetch(str);
        }

        py::list keys() const
        {
          py::list out;
          for (auto&[key, cellCache] : _cache)
            for (auto i = 0; i < cellCache.count(); ++i)
              out.append(py::wstr(_cache.writeKey(key, i)));
          return out;
        }

        ObjectCache<py::object, CacheUniquifier<py::object>> _cache;
        std::shared_ptr<const void> _workbookCloseHandler;
      };
    }
    ExcelObj pyCacheAdd(const py::object& obj, const wchar_t* caller)
    {
      return thePythonObjCache->_cache.add(py::object(obj), caller 
        ? CallerInfo(ExcelObj(caller))
        : CallerInfo());
    }
    bool pyCacheGet(const std::wstring_view& str, py::object& obj)
    {
      const auto* p = thePythonObjCache->_cache.fetch(str);
      if (p)
        obj = *p;
      return p;
    }

    namespace
    {
      static int theBinder = addBinder([](py::module& mod)
      {
        py::class_<PyCache>(mod, "ObjectCache")
          .def("add", &PyCache::add, py::arg("obj"), py::arg("tag")=nullptr)
          .def("remove", &PyCache::remove, py::arg("ref"))
          .def("get", &PyCache::get, py::arg("ref"))
          .def("contains", &PyCache::contains, py::arg("ref"))
          .def("keys", &PyCache::keys)
          .def("__contains__", &PyCache::contains)
          .def("__getitem__", &PyCache::get)
          .def("__call__", &PyCache::add, py::arg("obj"), py::arg("tag") = nullptr);
        mod.add_object("cache", py::cast(new PyCache()));
      });
    }
} }