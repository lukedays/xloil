#include "ComAddin.h"
#include <xlOil/ExcelTypeLib.h>
#include "ClassFactory.h"
#include "Connect.h"
#include "RibbonExtensibility.h"
#include "CustomTaskPane.h"
#include <xlOil/State.h>
#include <xlOil/Log.h>
#include <xlOil/ExcelApp.h>
#include <xlOil/Ribbon.h>
#include <xlOil/Events.h>
#include <map>
#include <functional>

using std::wstring;
using std::map;
using std::vector;
using std::shared_ptr;
using std::unique_ptr;
using namespace Office;

namespace xloil
{
  namespace COM
  {
    struct ComAddinEvents
    {
      void OnDisconnection(bool /*excelClosing*/) {}
      void OnAddInsUpdate() { Event::ComAddinsUpdate().fire(); }
      void OnBeginShutdown() {}
    };


    class __declspec(novtable)
      CustomTaskPaneConsumerImpl :
        public CComObjectRootEx<CComSingleThreadModel>,
        public NoIDispatchImpl<ICustomTaskPaneConsumer>
    {
    public:
      HRESULT __stdcall raw_CTPFactoryAvailable(ICTPFactory* ctpfactory) override
      {
        factory = ctpfactory;
        return S_OK;
      }

      HRESULT _InternalQueryInterface(REFIID riid, void** ppv) throw()
      {
        *ppv = NULL;
        if (riid == IID_IUnknown || riid == IID_IDispatch
          || riid == __uuidof(ICustomTaskPaneConsumer))
        {
          *ppv = this;
          AddRef();
          return S_OK;
        }
        return E_NOINTERFACE;
      }

      STDMETHOD(Invoke)(
        _In_ DISPID dispidMember,
        _In_ REFIID /*riid*/,
        _In_ LCID /*lcid*/,
        _In_ WORD /*wFlags*/,
        _In_ DISPPARAMS* pdispparams,
        _Out_opt_ VARIANT* /*pvarResult*/,
        _Out_opt_ EXCEPINFO* /*pexcepinfo*/,
        _Out_opt_ UINT* /*puArgErr*/) override
      {
        // Remember the args are in reverse order
        auto* rgvarg = pdispparams->rgvarg;

        if (dispidMember == 1)
        {
          return raw_CTPFactoryAvailable((Office::ICTPFactory*)rgvarg[0].pdispVal);
        }
        else
        {
          XLO_ERROR("Internal Error: unknown dispid called on task pane consumer Invoke.");
          return E_FAIL;
        }
        return S_OK;
      }

      ICTPFactory* factory = nullptr;
    };

    // This class does not need a disp-interface
    class __declspec(novtable)
      ComAddinImpl :
        public CComObjectRootEx<CComSingleThreadModel>,
        public NoIDispatchImpl<AddInDesignerObjects::IDTExtensibility2>
    {
    public:
      HRESULT _InternalQueryInterface(REFIID riid, void** ppv) throw()
      {
        *ppv = NULL;
        if (riid == IID_IUnknown 
          || riid == __uuidof(AddInDesignerObjects::IDTExtensibility2))
        {
          auto p = (AddInDesignerObjects::IDTExtensibility2*)this;
          *ppv = p;
          p->AddRef();
          return S_OK;
        }
        else if (riid == __uuidof(IRibbonExtensibility))
        {
          if (ribbon)
            return ribbon->QueryInterface(riid, ppv);
        }
        else if (riid == __uuidof(ICustomTaskPaneConsumer))
        {
          return customTaskPane->QueryInterface(riid, ppv);
        }
        return E_NOINTERFACE;
      }
      virtual HRESULT __stdcall raw_OnConnection(
        /*[in]*/ IDispatch * /*Application*/,
        /*[in]*/ enum AddInDesignerObjects::ext_ConnectMode /*ConnectMode*/,
        /*[in]*/ IDispatch * /*AddInInst*/,
        /*[in]*/ SAFEARRAY**) override
      {
        return S_OK;
      }
      virtual HRESULT __stdcall raw_OnDisconnection(
        /*[in]*/ enum AddInDesignerObjects::ext_DisconnectMode RemoveMode,
        /*[in]*/ SAFEARRAY**) override
      {
        events->OnDisconnection(
          RemoveMode == AddInDesignerObjects::ext_DisconnectMode::ext_dm_HostShutdown);
        return S_OK;
      }
      virtual HRESULT __stdcall raw_OnAddInsUpdate(SAFEARRAY**) override
      {
        events->OnAddInsUpdate();
        return S_OK;
      }
      virtual HRESULT __stdcall raw_OnStartupComplete(SAFEARRAY**) override
      {
        return S_OK;
      }
      virtual HRESULT __stdcall raw_OnBeginShutdown(SAFEARRAY**) override
      {
        events->OnBeginShutdown();
        return S_OK;
      }

      IRibbonExtensibility* ribbon;
      CComPtr<ComObject<CustomTaskPaneConsumerImpl>> customTaskPane = new ComObject<CustomTaskPaneConsumerImpl>();
      unique_ptr<ComAddinEvents> events;
    };

    struct SetAutomationSecurity
    {
      SetAutomationSecurity(Office::MsoAutomationSecurity value)
      {
        _previous = excelApp().AutomationSecurity;
        excelApp().AutomationSecurity = value;
      }
      ~SetAutomationSecurity()
      {
        try
        {
          excelApp().AutomationSecurity = _previous;
        }
        catch (...)
        {
        }
      }
      Office::MsoAutomationSecurity _previous;
    };

    class ComAddinCreator : public IComAddin
    {
    private:
      auto& comAddinImpl() const
      {
        return _registrar.server();
      }

      RegisterCom<ComAddinImpl> _registrar;
      bool _connected = false;
      shared_ptr<IRibbon> _ribbon;
      COMAddIn* _comAddin = nullptr;

    public:
      ComAddinCreator(const wchar_t* name, const wchar_t* description)
        : _registrar(
          [](const wchar_t*, const GUID&) { return new CComObject<ComAddinImpl>(); },
          formatStr(L"%s.ComAddin", name ? name : L"xlOil").c_str())
      {
        // TODO: hook OnDisconnect to stop user from disabling COM stub.
        comAddinImpl().events.reset(new ComAddinEvents());

        if (!name)
          XLO_THROW("Com add-in name must be provided");

        // It's possible the addin has already been registered and loaded and 
        // is just being reinitialised, so we do findAddin twice
        auto& app = excelApp();

        SetAutomationSecurity setSecurity(
          Office::MsoAutomationSecurity::msoAutomationSecurityLow);

        findAddin(app);

        if (isConnected())
        {
          disconnect();
        }
        else
        {
          const auto addinPath = fmt::format(
            L"Software\\Microsoft\\Office\\Excel\\AddIns\\{0}", _registrar.progid());
          _registrar.writeRegistry(
            HKEY_CURRENT_USER, addinPath.c_str(), L"FriendlyName", name);
          _registrar.writeRegistry(
            HKEY_CURRENT_USER, addinPath.c_str(), L"LoadBehavior", (DWORD)0);
          if (description)
            _registrar.writeRegistry(
              HKEY_CURRENT_USER, addinPath.c_str(), L"Description", description);

          app.GetCOMAddIns()->Update();
          findAddin(app);
          if (!_comAddin)
            XLO_THROW(L"Add-in connect: could not find addin '{0}'", progid());
        }
      }

      ~ComAddinCreator()
      {
        try
        {
          disconnect();
        }
        catch (const std::exception& e)
        {
          XLO_ERROR("ComAddin failed to close: {0}", e.what());
        }
      }

      void findAddin(Excel::_Application& app)
      {
        auto ourProgid = _variant_t(progid());
        app.GetCOMAddIns()->raw_Item(&ourProgid, &_comAddin);
      }

      bool isConnected() const
      {
        try
        {
          return _comAddin
            ? (_comAddin->Connect == VARIANT_TRUE)
            : false;
        }
        XLO_RETHROW_COM_ERROR;
      }
      void connect() override
      {
        if (_connected)
          return;
        try
        {
          _comAddin->Connect = VARIANT_TRUE;
          _connected = true;
        }
        XLO_RETHROW_COM_ERROR;
      }

      void disconnect() override
      {
        if (!_connected)
          return;
        try
        {
          _comAddin->Connect = VARIANT_FALSE;
          _connected = false;
        }
        XLO_RETHROW_COM_ERROR;
      }

      virtual void setRibbon(
        const wchar_t* xml,
        const RibbonMap& mapper)
      {
        if (_connected)
          XLO_THROW("Can only set Ribbon when add-in is disconnected");
        _ribbon = createRibbon(xml, mapper);
        comAddinImpl().ribbon = _ribbon->getRibbon();
      }

      const wchar_t* progid() const override
      {
        return _registrar.progid();
      }
      void ribbonInvalidate(const wchar_t* controlId = 0) const override
      {
        if (_ribbon)
          _ribbon->invalidate(controlId);
      }
      bool ribbonActivate(const wchar_t* controlId) const override
      {
        return _ribbon
          ? _ribbon->activateTab(controlId)
          : false;
      }

      ICustomTaskPane* createTaskPane(const wchar_t* name) const override
      {
        return createCustomTaskPane(comAddinImpl().customTaskPane->factory, name);
      }
    };

    std::shared_ptr<IComAddin> createComAddin(
      const wchar_t* name, const wchar_t* description)
    {
      return std::make_shared<ComAddinCreator>(name, description);
    }
  }
}