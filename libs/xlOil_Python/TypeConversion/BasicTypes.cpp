#include "BasicTypes.h"
#include "PyCore.h"
#include "PyEvents.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using std::shared_ptr;

namespace xloil 
{
  namespace Python
  {
    const char* IPyFromExcel::name() const
    {
      return "type";
    }

    namespace
    {
      struct CustomReturnConverter
      {
        std::shared_ptr<const IPyToExcel> value;
      };
      CustomReturnConverter* theConverter = nullptr;
    }

    const IPyToExcel* detail::getCustomReturnConverter()
    {
      return theConverter->value.get();
    }

    namespace
    {
      /// <summary>
      /// Type converter which expects a cache reference string and rejects
      /// all other types.
      /// </summary>
      class PyCacheObject : public ExcelValVisitor<PyObject*>
      {
      public:
        using ExcelValVisitor::operator();
        static constexpr char* const ourName = "CacheObject";

        PyObject* operator()(const PStringRef& pstr) const
        {
          PyObject* _typeCheck = nullptr;

          pybind11::object cached;
          if (pyCacheGet(pstr.view(), cached))
          {
            // Type checking seems nice, but it's unpythonic to raise an error here
            if (_typeCheck && PyObject_IsInstance(cached.ptr(), _typeCheck) == 0)
              XLO_WARN(L"Found `{0}` in cache but type was expected", pstr.string());
            return cached.release().ptr();
          }
          return nullptr;
        }

        constexpr wchar_t* failMessage() const { return L"Expected cache string"; }
      };

      struct FromPyLong
      {
        auto operator()(const PyObject* obj) const
        {
          if (!PyLong_Check(obj))
            XLO_THROW("Expected python int, got '{0}'", pyToStr(obj));
          return ExcelObj(PyLong_AsLong((PyObject*)obj));
        }
      };
      struct FromPyFloat
      {
        auto operator()(const PyObject* obj) const
        {
          if (!PyFloat_Check(obj))
            XLO_THROW("Expected python float, got '{0}'", pyToStr(obj));
          return ExcelObj(PyFloat_AS_DOUBLE(obj));
        }
      };
      struct FromPyBool
      {
        auto operator()(const PyObject* obj) const
        {
          if (!PyBool_Check(obj))
            XLO_THROW("Expected python bool, got '{0}'", pyToStr(obj));
          return ExcelObj(PyObject_IsTrue((PyObject*)obj) > 0);
        }
      };

      struct FromPyToCache
      {
        auto operator()(const PyObject* obj) const
        {
          return pyCacheAdd(PyBorrow<>(const_cast<PyObject*>(obj)));
        }
      };

      /// <summary>
      /// Always returns a single cell value. Uses the Excel object cache for 
      /// returned arrays and the Python object cache for unconvertable objects
      /// </summary>
      struct FromPyToSingleValue
      {
        auto operator()(const PyObject* obj) const
        {
          ExcelObj excelObj(FromPyObj()(obj));
          if (excelObj.isType(ExcelType::ArrayValue))
            return std::move(excelObj);
          return makeCached<ExcelObj>(std::move(excelObj));
        }
      };

    }
    
#pragma region PYBIND_BINDINGS
    namespace
    {
      template <class T>
      void convertPy(pybind11::module& mod, const char* type)
      {
        bindPyConverter<PyFromExcelConverter<T>>(mod, type).def(py::init<>());
      }

      template <class T>
      void convertXl(pybind11::module& mod, const char* type)
      {
        bindXlConverter<PyFuncToExcel<T>>(mod, type).def(py::init<>());
      }

      static int theBinder = addBinder([](py::module& mod)
      {
        // Bind converters for standard types
        convertPy<PyFromInt>(mod, "int");
        convertPy<PyFromDouble>(mod, "float");
        convertPy<PyFromBool>(mod, "bool");
        convertPy<PyFromString>(mod, "str");
        convertPy<PyFromAny>(mod, "object");
        convertPy<PyCacheObject>(mod, "Cache");

        convertPy<PyFromIntUncached>(mod, XLOPY_UNCACHED_PREFIX "int");
        convertPy<PyFromDoubleUncached>(mod, XLOPY_UNCACHED_PREFIX "float");
        convertPy<PyFromBoolUncached>(mod, XLOPY_UNCACHED_PREFIX "bool");
        convertPy<PyFromStringUncached>(mod, XLOPY_UNCACHED_PREFIX "str");
        convertPy<PyFromAnyUncached>(mod, XLOPY_UNCACHED_PREFIX "object");

        convertXl<FromPyLong>(mod, "int");
        convertXl<FromPyFloat>(mod, "float");
        convertXl<FromPyBool>(mod, "bool");
        convertXl<FromPyString>(mod, "str");
        convertXl<FromPyToCache>(mod, "Cache");
        convertXl<FromPyToSingleValue>(mod, "SingleValue");

        py::class_<CustomReturnConverter>(mod, "_CustomReturnConverter")
          .def_readwrite("value", &CustomReturnConverter::value);
          
        theConverter = new CustomReturnConverter();
        mod.add_object("_return_converter_hook",
          py::cast(theConverter, py::return_value_policy::take_ownership));
      });
    }
#pragma endregion
  }
}
