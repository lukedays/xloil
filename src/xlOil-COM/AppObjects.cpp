#include <xloil/AppObjects.h>
#include <xlOil-COM/Connect.h>
#include <xlOil-COM/ComVariant.h>
#include <xlOil/ExcelTypeLib.h>
#include <xlOil/ExcelRange.h>
#include <xloil/Log.h>
#include <xloil/Throw.h>
#include <xloil/State.h>
#include <functional>
#include <comdef.h>

using std::shared_ptr;
using std::make_shared;
using std::vector;

namespace xloil
{
  namespace
  {
    template <class T>
    struct CollectionToVector
    {
      template <class V>
      vector<T> operator()(const V& collection) const
      {
        try
        {
          vector<T> result;
          const auto N = collection->Count;
          for (auto i = 1; i <= N; ++i)
            result.emplace_back(collection->GetItem(i));
          return std::move(result);
        }
        XLO_RETHROW_COM_ERROR;
      }
    };

    _variant_t stringToVariant(const std::wstring_view& str)
    {
      auto variant = COM::stringToVariant(str);
      return _variant_t(variant, false);
    }
  }

  Application& excelApp() noexcept
  {
    static Application theApp(&COM::attachedExcelApp());
    return theApp;
  }

  IAppObject::~IAppObject()
  {
    if (_ptr)
      _ptr->Release();
  }

  void IAppObject::init(IDispatch* ptr)
  {
    _ptr = ptr;
    if (ptr)
      ptr->AddRef();
  }

  void IAppObject::assign(const IAppObject& that)
  {
    if (_ptr) _ptr->Release();
    _ptr = that._ptr;
    _ptr->AddRef();
  }



  Application::Application(Excel::_Application* app)
    : IAppObject(app)
  {
  }


  Application::Application(size_t hWnd)
    : IAppObject([hWnd]() {
    auto p = COM::applicationObjectFromWindow((HWND)hWnd);
    if (!p)
      throw ComConnectException("Window not found");
    return p;
  }())
  {
  }

  //namespace
  //{
  //  Excel::_Application* workbookFinder(const wchar_t* workbook)
  //  {
  //    HWND xlmain = 0;
  //    while ((xlmain = COM::nextExcelMainWindow(xlmain)) != 0)
  //    {
  //      auto xlApp = Application(COM::applicationObjectFromWindow(xlmain));
  //      auto wb = xlApp.tryGetWorkbook(workbook);
  //      if (wb)
  //      {
  //        wb->Release();
  //        return &xlApp.com();
  //      }
  //    }
  //    return nullptr;
  //  }
  //}
  //Application::Application(const wchar_t* workbook)
  //  : IAppObject(workbookFinder(workbook))
  //{
  //}

  std::wstring Application::name() const
  {
    return com().Name.GetBSTR();
  }


  ExcelWindow::ExcelWindow(const std::wstring_view& caption, Application app)
  {
    try
    {
      if (caption.empty())
        init(app.com().ActiveWindow);
      else
        init(app.com().Windows->GetItem(stringToVariant(caption)));
    }
    XLO_RETHROW_COM_ERROR;
  }

  size_t ExcelWindow::hwnd() const
  {
    return (size_t)com().Hwnd;
  }

  std::wstring ExcelWindow::name() const
  {
    return com().Caption.bstrVal;
  }

  ExcelWorkbook ExcelWindow::workbook() const
  {
    try
    {
      return ExcelWorkbook(Excel::_WorkbookPtr(com().Parent));
    }
    XLO_RETHROW_COM_ERROR;
  }

  ExcelWorkbook::ExcelWorkbook(const std::wstring_view& name, Application app)
  {
    try
    {
      if (name.empty())
        init(app.com().ActiveWorkbook);
      else
        init(app.com().Workbooks->GetItem(stringToVariant(name)));
    }
    XLO_RETHROW_COM_ERROR;
  }

  std::wstring ExcelWorkbook::name() const
  {
    return com().Name.GetBSTR();
  }

  std::wstring ExcelWorkbook::path() const
  {
    return com().Path.GetBSTR();
  }
  std::vector<ExcelWindow> ExcelWorkbook::windows() const
  {
    return CollectionToVector<ExcelWindow>()(com().Windows);
  }

  void ExcelWorkbook::activate() const
  {
    com().Activate();
  }

  vector<ExcelWorksheet> ExcelWorkbook::worksheets() const
  {
    try
    {
      vector<ExcelWorksheet> result;
      const auto N = com().Worksheets->Count;
      for (auto i = 1; i <= N; ++i)
        result.push_back((Excel::_Worksheet*)(IDispatch*)com().Worksheets->GetItem(i));
      return std::move(result);
    }
    XLO_RETHROW_COM_ERROR;
  }
  ExcelWorksheet ExcelWorkbook::worksheet(const std::wstring_view& name) const
  {
    try
    {
      return (Excel::_Worksheet*)(IDispatch*)(com().Worksheets->GetItem(stringToVariant(name)));
    }
    XLO_RETHROW_COM_ERROR;
  }

  std::wstring ExcelWorksheet::name() const
  {
    return com().Name.GetBSTR();
  }

  ExcelWorkbook ExcelWorksheet::parent() const
  {
    return ExcelWorkbook((Excel::_Workbook*)(IDispatch*)com().Parent);
  }

  ExcelRange ExcelWorksheet::range(
    int fromRow, int fromCol,
    int toRow, int toCol) const
  {
    try
    {
      if (toRow == Range::TO_END)
        toRow = com().Rows->GetCount();
      if (toCol == Range::TO_END)
        toCol = com().Columns->GetCount();

      auto r = com().GetRange(
        com().Cells->Item[fromRow + 1][fromCol + 1],
        com().Cells->Item[toRow + 1][toCol + 1]);
      return ExcelRange(r);
    }
    XLO_RETHROW_COM_ERROR;
  }

  ExcelRange ExcelWorksheet::range(const std::wstring_view& address) const
  {
    auto fullAddress = std::wstring(com().Name);
    fullAddress += '!';
    fullAddress += address;
    return ExcelRange(fullAddress.c_str());
  }
  ExcelObj ExcelWorksheet::value(Range::row_t i, Range::col_t j) const
  {
    return COM::variantToExcelObj(com().Cells->Item[i][j]);
  }
  void ExcelWorksheet::activate()
  {
    try
    {
      com().Activate();
    }
    XLO_RETHROW_COM_ERROR;
  }
  void ExcelWorksheet::calculate()
  {
    try
    {
      com().Calculate();
    }
    XLO_RETHROW_COM_ERROR;
  }


    namespace
    {
      template <typename F, typename T, std::size_t N, std::size_t... Idx>
      decltype(auto) appRun_impl(F func, T(&args)[N], std::index_sequence<Idx...>) {
        return excelApp().com().Run(func, args[Idx]...);
      }

      template <typename T, std::size_t N>
      decltype(auto) appRun(const wchar_t* func, T(&args)[N]) {
        return appRun_impl(func, args, std::make_index_sequence<N>{});
      }
    }

  ExcelObj Application::Run(
    const std::wstring& func, 
    const size_t nArgs, 
    const ExcelObj* args[])
  {
    if (nArgs > 30)
      XLO_THROW("Application::Run maximum number of args is 30");

    static _variant_t vArgs[30] = {
      vtMissing, vtMissing, vtMissing, vtMissing, vtMissing,
      vtMissing, vtMissing, vtMissing, vtMissing, vtMissing,
      vtMissing, vtMissing, vtMissing, vtMissing, vtMissing,
      vtMissing, vtMissing, vtMissing, vtMissing, vtMissing,
      vtMissing, vtMissing, vtMissing, vtMissing, vtMissing,
      vtMissing, vtMissing, vtMissing, vtMissing, vtMissing
    };

    // The construction of 'cleanup' is all noexcept
    auto finally = [begin = vArgs, end = vArgs + nArgs](void*)
    {
      for (auto i = begin; i != end; ++i)
        *i = vtMissing;
    };
    std::unique_ptr<void, decltype(finally)> cleanup((void*)1, finally);

    for (size_t i = 0; i < nArgs; ++i)
      COM::excelObjToVariant(&vArgs[i], *args[i], true);

    try
    {
      auto result = appRun(func.c_str(), vArgs);
      return COM::variantToExcelObj(result);
    }
    XLO_RETHROW_COM_ERROR;
  }

  bool Workbooks::tryGet(const wchar_t* workbookName, ExcelWorkbook& wb) const
  {
    // See other possibility here. Seems a bit crazy?
    // https://stackoverflow.com/questions/9373082/detect-whether-excel-workbook-is-already-open
    try
    {
      wb = Workbooks().get(workbookName);
      return true;
    }
    catch (_com_error& error)
    {
      if (error.Error() == DISP_E_BADINDEX)
        return false;
      XLO_THROW(L"COM Error {0:#x}: {1}", (size_t)error.Error(), error.ErrorMessage());
    }
  }

  bool Windows::tryGet(const wchar_t* caption, ExcelWindow& window) const
  {
    // See other possibility here. Seems a bit crazy?
    // https://stackoverflow.com/questions/9373082/detect-whether-excel-workbook-is-already-open
    try
    {
      window = Windows().get(caption);
      return true;
    }
    catch (_com_error& error)
    {
      if (error.Error() == DISP_E_BADINDEX)
        return false;
      XLO_THROW(L"COM Error {0:#x}: {1}", (size_t)error.Error(), error.ErrorMessage());
    }
  }

  ExcelWorksheet Application::ActiveWorksheet() const
  {
    try
    {
      Excel::_Worksheet* sheet = nullptr;
      com().ActiveSheet->QueryInterface(&sheet);
      return ExcelWorksheet(sheet);
    }
    XLO_RETHROW_COM_ERROR;
  }

  Workbooks::Workbooks(Application app)
    : app(app)
  {}

  ExcelWorkbook Workbooks::active() const
  {
    return ExcelWorkbook(std::wstring_view(), app);
  }
  std::vector<ExcelWorkbook> Workbooks::list() const
  {
    return CollectionToVector<ExcelWorkbook>()(app.com().Workbooks);
  }
  size_t Workbooks::count() const
  {
    return app.com().Workbooks->Count;
  }

  Windows::Windows(Application app)
    : app(app)
  {}

  ExcelWindow Windows::active() const
  {
    return ExcelWindow(std::wstring_view(), app);
  }

  std::vector<ExcelWindow> Windows::list() const
  {
    return CollectionToVector<ExcelWindow>()(app.com().Windows);
  }

  size_t Windows::count() const
  {
    return app.com().Windows->Count;
  }

  void Application::allowEvents(bool value)
  {
    try
    {
      com().EnableEvents = _variant_t(value);
    }
    XLO_RETHROW_COM_ERROR;
  }
}
