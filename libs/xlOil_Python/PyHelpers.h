#pragma once
#include "CPython.h"
#include <xloil/StringUtils.h>
#include <xloil/Throw.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>

// Seems useful, wonder why it's not in the API?
#define PyIterable_Check(obj) \
    ((obj)->ob_type->tp_iter != NULL && \
     (obj)->ob_type->tp_iter != &_PyObject_NextNotImplemented)

namespace pybind11
{
  // Adds a logically missing wstr class to pybind11
  class wstr : public object {
  public:
    PYBIND11_OBJECT_CVT(wstr, object, detail::PyUnicode_Check_Permissive, raw_str)

    wstr(const wchar_t *c, size_t n)
      : object(PyUnicode_FromWideChar(c, (ssize_t)n), stolen_t{}) 
    {
      if (!m_ptr) pybind11_fail("Could not allocate string object!");
    }

    // 'explicit' is explicitly omitted from the following constructors to allow implicit 
    // conversion to py::str from C++ string-like objects
    wstr(const wchar_t *c = L"")
      : object(PyUnicode_FromWideChar(c, -1), stolen_t{})
    {
      if (!m_ptr) pybind11_fail("Could not allocate string object!");
    }

    wstr(const std::wstring &s) : wstr(s.data(), s.size()) { }

    // Not sure how to implement
    //explicit str(const bytes &b);

    explicit wstr(handle h) : object(raw_str(h.ptr()), stolen_t{}) { }

    operator std::wstring() const {
      if (!PyUnicode_Check(m_ptr))
        pybind11_fail("Unable to extract string contents!");
      ssize_t length;
      wchar_t* buffer = PyUnicode_AsWideCharString(ptr(), &length);
      return std::wstring(buffer, (size_t)length);
    }

    template <typename... Args>
    wstr format(Args &&...args) const {
      return attr("format")(std::forward<Args>(args)...);
    }

  private:
    /// Return string representation -- always returns a new reference, even if already a str
    static PyObject *raw_str(PyObject *op) {
      PyObject *str_value = PyObject_Str(op);
#if PY_MAJOR_VERSION < 3
      if (!str_value) throw error_already_set();
      PyObject *unicode = PyUnicode_FromEncodedObject(str_value, "utf-8", nullptr);
      Py_XDECREF(str_value); str_value = unicode;
#endif
      return str_value;
    }
  };
}

namespace xloil
{
  namespace Python
  {
    inline PyObject* PyCheck(PyObject* obj)
    {
      if (!obj)
        throw pybind11::error_already_set();
      return obj;
    }
    inline PyObject* PyCheck(int ret)
    {
      if (ret != 0)
        throw pybind11::error_already_set();
      return 0;
    }
    template<class TType = pybind11::object> inline TType PySteal(PyObject* obj)
    {
      if (!obj)
        throw pybind11::error_already_set();
      return pybind11::reinterpret_steal<TType>(obj);
    }
    template<class TType = pybind11::object> inline TType PyBorrow(PyObject* obj)
    {
      if (!obj)
        throw pybind11::error_already_set();
      return pybind11::reinterpret_borrow<TType>(obj);
    }
    inline std::wstring pyErrIfOccurred()
    {
      return PyErr_Occurred() 
        ? utf8ToUtf16(pybind11::detail::error_string().c_str()) 
        : std::wstring();
    }
    inline auto pyToStr(const PyObject* p)
    {
      // Is morally const: py::handle doesn't change refcount
      return (std::string)pybind11::str(pybind11::handle((PyObject*)p));
    }

    std::wstring pyToWStr(const PyObject* p);
    inline std::wstring pyToWStr(const pybind11::object& p) { return pyToWStr(p.ptr()); }

    /// <summary>
    /// Reads an argument to __getitem__ i.e. [] using the following rules
    ///     None => entire array
    ///     Slice [a:b] => compute indices using python rules
    ///     int => single value (0-based)
    /// Modifies the <param ref="from"/> and <param ref="to"/> arguments
    /// to indicate the extent of the sliced array. Only handles slices with
    /// stride = 1.
    /// </summary>
    /// <param name="index"></param>
    /// <param name="size">The size of the object being indexed</param>
    /// <param name="from"></param>
    /// <param name="to"></param>
    /// <returns>Returns true if only a single element is accessed</returns>
    bool getItemIndexReader1d(
      const pybind11::object& index,
      const size_t size, size_t& from, size_t& to);

    /// <summary>
    /// Take a 2-tuple of indeices and applies <see ref="getItemIndexReader1d"/> in 
    /// each dimension
    /// </summary>
    /// <param name="loc"></param>
    /// <param name="nRows">The first dimension of the object being indexed</param>
    /// <param name="nCols">The second dimension of the object being indexed</param>
    /// <param name="fromRow"></param>
    /// <param name="fromCol"></param>
    /// <param name="toRow"></param>
    /// <param name="toCol"></param>
    /// <returns>Returns true if only a single element is accessed/returns>
    bool getItemIndexReader2d(
      const pybind11::tuple& loc,
      const size_t nRows, const size_t nCols,
      size_t& fromRow, size_t& fromCol,
      size_t& toRow, size_t& toCol);

    /// <summary>
      /// Holds a py::object and ensures the GIL is held when the holder is destroyed
      /// and the underlying py::object is decref'd 
      /// </summary>
    class PyObjectHolder : public pybind11::detail::object_api<PyObjectHolder>
    {
      pybind11::object _obj;
    public:
      PyObjectHolder(const pybind11::object& obj)
        : _obj(obj)
      {}
      ~PyObjectHolder()
      {
        pybind11::gil_scoped_acquire getGil;
        _obj = pybind11::none();
      }
      operator pybind11::object() const { return _obj; }

      /// Return the underlying ``PyObject *`` pointer
      PyObject* ptr() const { return _obj.ptr(); }
      PyObject*& ptr() { return _obj.ptr(); }
    };


  }
}