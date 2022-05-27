#include "WorkbookScopeFunctions.h"
#include "Connect.h"
#include "ComVariant.h"
#include <xlOil-Dynamic/LocalFunctions.h>
#include <xlOil/ExcelRange.h>
#include <xlOil/Register.h>
#include <xlOilHelpers/Environment.h>
#include <xloil/FuncSpec.h>
#include <xloil/ExcelCall.h>
#include <xloil/State.h>
#include <xlOil/ExcelTypeLib.h>
#include <xlOil/ExcelThread.h>
#include <xlOil/AppObjects.h>
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
      auto excelVersion = App::internals().version;
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

      template <typename S, typename... Args>
      void write(const S& fmtStr, Args&&... args)
      {
        mod->InsertLines(line++, fmt::format(fmtStr, std::forward<Args>(args)...).c_str());
      }
    };

    void writeLocalFunctionsToVBA(
      const wchar_t* workbookName,
      const vector<shared_ptr<const LocalWorksheetFunc>>& registeredFuncs,
      const bool append)
    {
      try
      {
        constexpr char* ourModuleName = "xlOil_AutoGenerated";

        // Check we have trusted access to VBA object model
        static bool registryChecked = checkRegistryKeys();

        auto workbook = App::Workbooks::get(workbookName);

        auto vbProj = workbook.com().VBProject;

        struct _VBComponent* vbFound = 0;
        vbProj->VBComponents->raw_Item(_variant_t(ourModuleName), &vbFound);
        _VBComponentPtr vbMod;
        auto startLine = 1;
        if (!vbFound)
        {
          vbMod = vbProj->VBComponents->Add(vbext_ct_StdModule);
          vbMod->PutName(ourModuleName);
        }
        else
        {
          vbMod = vbProj->VBComponents->Item(ourModuleName);
          if (!append)
            vbMod->CodeModule->DeleteLines(1, vbMod->CodeModule->CountOfLines);
          else
            startLine = vbMod->CodeModule->CountOfLines + 1;
        }

        Writer writer{ startLine, vbMod->CodeModule };
        if (!append)
          writer.write(L"Declare PtrSafe Function localFunctionEntryPoint "
            "Lib \"xloil.dll\" "
            "(ByRef funcId as LongPtr, "
            " ByRef ret as variant, "
            " ByRef args as variant) as Long");

        for (size_t i = 0; i < registeredFuncs.size(); ++i)
        {
          auto& func = *registeredFuncs[i]->info();
          // We declare all args as optional variant and let the called 
          // function handle things.
          wstring args, optionalArgs;
          for (auto& arg : func.args)
          {
            args += arg.name + L',';
            optionalArgs += L"Optional " + arg.name + L",";
          }

          // Drop final comma
          if (!optionalArgs.empty()) optionalArgs.pop_back();
          if (!args.empty()) args.pop_back();

          // We write:
          // 
          // Public Function name(Optional arg0, Optional arg1,...)
          //   Dim args: args = Array(arg0, arg1, ...)
          //   localFunctionEntryPoint workbook, 'name, name, args
          // End Function
          // 
          // For a command we replace Function with Sub and add a dummy return
          // as localFunctionEntryPoint expects a return value
          //
          const bool isSub = (func.options & FuncInfo::COMMAND) != 0;
          const auto& name = func.name;
          const auto declaration = isSub ? L"Sub" : L"Function";
          const auto retVar = isSub ? L"dummy" : name;
          const auto funcId = registeredFuncs[i]->registerId();

          writer.write(L"Public {2} {0}({1})", name, optionalArgs, declaration);
          writer.write(L"  Dim args: args=Array({0})", args);
          if (isSub)
            writer.write(L"  Dim dummy");
          writer.write(L"  localFunctionEntryPoint {0}, {1}, args",
            funcId, retVar);
          writer.write(L"End {0}", declaration);
        }
      }
      XLO_RETHROW_COM_ERROR;
    }
  }
}
