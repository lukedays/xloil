#include "xloil/Date.h"
#include "BasicTypes.h"
#include "InjectedModule.h"
#include <Python.h>
#include <datetime.h>

namespace py = pybind11;

namespace xloil
{
  namespace Python
  {
    void importDatetime()
    {
      PyDateTime_IMPORT;
    }

    bool isPyDate(PyObject* p)
    {
      return (PyDate_CheckExact(p) || PyDateTime_CheckExact(p));
    }

    ExcelObj pyDateToExcel(PyObject* p)
    {
      if (PyDateTime_CheckExact(p))
      {
        auto serial = excelSerialDateFromDMYHMS(
          PyDateTime_GET_DAY(p), PyDateTime_GET_MONTH(p), PyDateTime_GET_YEAR(p),
          PyDateTime_DATE_GET_HOUR(p), PyDateTime_DATE_GET_MINUTE(p), PyDateTime_DATE_GET_SECOND(p),
          PyDateTime_DATE_GET_MICROSECOND(p)
        );
        return ExcelObj(serial);
      }
      else
      {
        auto serial = excelSerialDateFromDMY(
          PyDateTime_GET_DAY(p), PyDateTime_GET_MONTH(p), PyDateTime_GET_YEAR(p));
        return ExcelObj(serial);
      }
    }

    template<class TSuper=nullptr_t>
    class PyFromDate : public PyFromCache<NotNull<TSuper, PyFromDate<>>>
    {
    public:
      // TODO: string->date conversion, slow but useful
      PyObject* fromInt(int x) const 
      {
        int day, month, year;
        excelSerialDateToDMY(x, day, month, year);
        return PyDate_FromDate(year, month, day);
      }
      PyObject* fromDouble(double x) const
      {
        return fromInt(int(x));
      }
      PyObject* fromString(const wchar_t* buf, size_t len) const
      {
        std::tm tm;
        if (stringToDateTime(std::wstring_view(buf, len), tm))
          return PyDate_FromDate(tm.tm_year, tm.tm_mon, tm.tm_yday);
        return PyFromCache::fromString(buf, len);
      }
    };
    class PyFromDateTime : public PyFromDate<PyFromDateTime>
    {
    public:
      PyObject* fromDouble(double x) const
      {
        int day, month, year, hours, mins, secs, usecs;
        excelSerialDatetoDMYHMS(x, day, month, year, hours, mins, secs, usecs);
        return PyDateTime_FromDateAndTime(year, month, day, hours, mins, secs, usecs);
      }
      PyObject* fromString(const wchar_t* buf, size_t len) const
      {
        std::tm tm;
        if (stringToDateTime(std::wstring_view(buf, len), tm))
          return PyDateTime_FromDateAndTime(
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, 0);
        return PyFromCache::fromString(buf, len);
      }
    };
    class XlFromDate : public IConvertToExcel<PyObject>
    {
    public:
      virtual ExcelObj operator()(const PyObject& obj) const override
      {
        return pyDateToExcel(const_cast<PyObject*>(&obj));
      }
    };

    namespace
    {
      template<class T>
      void declare2(pybind11::module& mod, const char* name)
      {
        py::class_<T, IPyToExcel, shared_ptr<T>>(mod, name)
          .def(py::init<>());
      }
      static int theBinder = addBinder([](py::module& mod)
      {
        bindFrom<PyFromExcel<PyFromDateTime>>(mod, "datetime").def(py::init<>());
        bindFrom<PyFromExcel<PyFromDate<>>>(mod, "date").def(py::init<>());
        bindTo<XlFromDate>(mod, "date").def(py::init<>());
      });
    }
  }
}