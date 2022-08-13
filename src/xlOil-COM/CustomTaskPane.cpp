#include "ClassFactory.h"
#include "TaskPaneHostControl.h"
#include <xlOil/ExcelTypeLib.h>
#include <xlOil/AppObjects.h>
#include <xlOil/ExcelUI.h>
#include <xloil/Throw.h>
#include <xloil/Log.h>
#include <xloil/State.h>

using std::shared_ptr;
using std::make_shared;

namespace xloil
{
  namespace COM
  {
    class CustomTaskPaneEventHandler
      : public ComEventHandler<
          NoIDispatchImpl<ComObject<Office::_CustomTaskPaneEvents>>, Office::_CustomTaskPaneEvents>
    {
    public:
      CustomTaskPaneEventHandler(
        ICustomTaskPane& parent, 
        shared_ptr<ICustomTaskPaneEvents> handler)
        : _parent(parent)
        , _handler(handler)
      {}

      virtual ~CustomTaskPaneEventHandler() noexcept
      {}

      STDMETHOD(Invoke)(DISPID dispidMember, REFIID /*riid*/,
        LCID /*lcid*/, WORD /*wFlags*/, DISPPARAMS* pdispparams, VARIANT* /*pvarResult*/,
        EXCEPINFO* /*pexcepinfo*/, UINT* /*puArgErr*/)
      {
        try
        {
          auto* rgvarg = pdispparams->rgvarg;

          // These dispids are copied from oleview and are in the same order as listed there
          switch (dispidMember)
          {
          case 1:
            VisibleStateChange((_CustomTaskPane*)rgvarg[0].pdispVal);
            break;
          case 2:
            DockPositionStateChange((_CustomTaskPane*)rgvarg[0].pdispVal);
            break;
          }
        }
        catch (const std::exception& e)
        {
          XLO_ERROR("Error during COM event handler callback: {0}", e.what());
        }

        return S_OK;
      }

      void destroy() noexcept
      {
        try
        {
          _handler->onDestroy();
          disconnect();
        }
        catch (const std::exception& e)
        {
          XLO_ERROR(e.what());
        }
      }

    private:
      HRESULT VisibleStateChange(
        struct _CustomTaskPane* /*CustomTaskPaneInst*/)
      {
        _handler->onVisible(_parent.getVisible());
        return S_OK;
      }
      HRESULT DockPositionStateChange(
        struct _CustomTaskPane* /*CustomTaskPaneInst*/)
      {
        _handler->onDocked();
        return S_OK;
      }

    private:
      ICustomTaskPane& _parent;
      shared_ptr<ICustomTaskPaneEvents> _handler;
    };

    class CustomTaskPaneCreator : public ICustomTaskPane
    {
      Office::_CustomTaskPanePtr _pane;
      CComPtr<CustomTaskPaneEventHandler> _paneEvents;
      CComQIPtr<ITaskPaneHostControl> _hostingControl;

    public:
      CustomTaskPaneCreator(
        Office::ICTPFactory& ctpFactory, 
        const wchar_t* name,
        const IDispatch* window,
        const wchar_t* progId)
      {
        XLO_DEBUG(L"Creating Custom Task Pane '{}'", name);
        // Pasing vtMissing causes the pane to be attached to ActiveWindow
        auto targetWindow = window ? _variant_t(window) : vtMissing;
        _pane = ctpFactory.CreateCTP(
          progId ? progId : taskPaneHostControlProgId(),
          name, 
          targetWindow);
        if (!progId)
          _hostingControl = content();
      }

      ~CustomTaskPaneCreator()
      {
        destroy();
      }

      IDispatch* content() const override
      {
        try
        {
          return _pane->ContentControl;
        }
        XLO_RETHROW_COM_ERROR;
      }

      ExcelWindow window() const override
      {
        try
        {
          return ExcelWindow(Excel::WindowPtr(_pane->Window));
        }
        XLO_RETHROW_COM_ERROR;
      }

      void setVisible(bool value) override
      { 
        try
        {
          _pane->Visible = value;
        }
        XLO_RETHROW_COM_ERROR;
      }
      bool getVisible() override
      {
        try
        {
          return _pane->Visible;
        }
        XLO_RETHROW_COM_ERROR;
      }
      std::pair<int, int> getSize() override
      {
        try
        {
          return std::make_pair(_pane->Width, _pane->Height);
        }
        XLO_RETHROW_COM_ERROR;
      }
      void setSize(int width, int height) override
      {
        try
        {
          _pane->Width = width;
          _pane->Height = height;
        }
        XLO_RETHROW_COM_ERROR;
      }
      DockPosition getPosition() const override
      {
        try
        {
          return DockPosition(_pane->DockPosition);
        }
        XLO_RETHROW_COM_ERROR;
      }
      void setPosition(DockPosition pos) override
      {
        try
        {
          _pane->DockPosition = (Office::MsoCTPDockPosition)pos;
        }
        XLO_RETHROW_COM_ERROR;
      }

      std::wstring getTitle() const
      {
        try
        {
          return _pane->Title.GetBSTR();
        }
        XLO_RETHROW_COM_ERROR;
      }

      void destroy() override
      {
        XLO_DEBUG(L"Destroying Custom Task Pane '{}'", getTitle());
        if (_hostingControl)
          _hostingControl.Release();
        if (_paneEvents)
        {
          _paneEvents->destroy();
          _paneEvents.Release();
        }
        _pane->Delete();
      }

      void listen(const std::shared_ptr<ICustomTaskPaneEvents>& events) override
      {
        _paneEvents = CComPtr<CustomTaskPaneEventHandler>(
          new CustomTaskPaneEventHandler(*this, events));
        _paneEvents->connect(_pane);
      }

      void attach(size_t hwnd) override
      {
        if (_hostingControl)
          _hostingControl->AttachWindow((HWND)hwnd);
      }
    };

    ICustomTaskPane* createCustomTaskPane(
      Office::ICTPFactory& ctpFactory, 
      const wchar_t* name,
      const IDispatch* window,
      const wchar_t* progId)
    {
      try
      {
        return new CustomTaskPaneCreator(ctpFactory, name, window, progId);
      }
      XLO_RETHROW_COM_ERROR;
    }
  }
}
