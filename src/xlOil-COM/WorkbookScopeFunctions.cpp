
#include "Connect.h"
#include "ComVariant.h"
#include <xlOil/ExcelRange.h>
#include <xlOil/Register.h>
#include <xlOilHelpers/Environment.h>
#include <xloil/FuncSpec.h>
#include <xloil/ExcelCall.h>
#include <xloil/State.h>
#include <xlOil/ExcelTypeLib.h>
#include <xlOil/ExcelApp.h>
#include <vector>
#include <memory>

using std::vector;
using std::shared_ptr;
using std::wstring;
using namespace VBIDE;

namespace xloil
{
  namespace COM
  {
    bool checkRegistryKeys()
    {
      auto excelVersion = State::excelState().version;
      auto regKey = fmt::format(L"Software\\Microsoft\\Office\\{0}.0\\Excel\\Security\\AccessVBOM", excelVersion);
      DWORD currentUser = 666, localMachine = 666;
      getWindowsRegistryValue(L"HKCU", regKey.c_str(), currentUser);
      getWindowsRegistryValue(L"HKLM", regKey.c_str(), localMachine);
      if (currentUser == 0 || localMachine == 0)
        XLO_THROW("Allow access to VBA Object Model in "
          "File > Options > Trust Center > Trust Center Settings > Macro Settings");
      return true;
    }

    struct Writer
    {
      int line;
      VBIDE::_CodeModulePtr mod;
      void write(const wchar_t* str)
      {
        mod->InsertLines(line++, str);
      }

      void write(const wstring& str)
      {
        mod->InsertLines(line++, str.c_str());
      }
    };

    void writeLocalFunctionsToVBA(
      const wchar_t* workbookName,
      const vector<shared_ptr<const WorksheetFuncSpec>>& registeredFuncs)
    {
      try
      {
        constexpr char* ourModuleName = "xlOil_AutoGenerated";

        // Check we have trusted access to VBA object model
        static bool registryChecked = checkRegistryKeys();

        auto workbook = excelApp().Workbooks->GetItem(_variant_t(workbookName));

        auto vbProj = workbook->VBProject;

        struct _VBComponent* vbFound = 0;
        vbProj->VBComponents->raw_Item(_variant_t(ourModuleName), &vbFound);
        _VBComponentPtr vbMod;
        if (!vbFound)
        {
          vbMod = vbProj->VBComponents->Add(vbext_ct_StdModule);
          vbMod->PutName(ourModuleName);
        }
        else
        {
          vbMod = vbProj->VBComponents->Item(ourModuleName);
          vbMod->CodeModule->DeleteLines(1, vbMod->CodeModule->CountOfLines);
        }

        Writer writer{ 1, vbMod->CodeModule };
        writer.write(L"Declare PtrSafe Function localFunctionEntryPoint "
          "Lib \"xloil.dll\" "
          "(ByRef workbookName as variant, "
          " ByRef funcName as variant, "
          " ByRef ret as variant, "
          " ByRef args as variant) as Long");

        for (size_t i = 0; i < registeredFuncs.size(); ++i)
        {
          auto& func = *registeredFuncs[i];
          // We declare all args as optional variant and let the called 
          // function handle things.
          wstring args, optionalArgs;
          for (auto& arg : func.info()->args)
          {
            args += arg.name + L',';
            optionalArgs += L"Optional " + arg.name + L",";
          }

          // Drop final comma
          if (!optionalArgs.empty()) optionalArgs.pop_back();
          if (!args.empty()) args.pop_back();

          // We write:
          // Public Function name(Optional arg0, Optional arg1,...)
          //   Dim args: args = Array(arg0, arg1, ...)
          //   localFunctionEntryPoint workbook, name, name, args
          // End Function

          auto& name = func.name();

          writer.write(fmt::format(L"Public Function {0}({1})", name, optionalArgs).c_str());
          writer.write(fmt::format(L"  Dim args: args=Array({0})", args));
          writer.write(fmt::format(L"  localFunctionEntryPoint \"{1}\", \"{2}\", {0}, args",
            name, workbookName, name));
          writer.write(L"End Function");
        }
      }
      XLO_RETHROW_COM_ERROR;
    }
  }
}
