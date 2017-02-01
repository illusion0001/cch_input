#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vd2/plugin/vdplugin.h>
#include <vd2/plugin/vdinputdriver.h>
#include <vd2/VDXFrame/VideoFilterDialog.h>
#include <string>
#include "InputFile2.h"
#include "export.h"
#include "cineform.h"
#include "a_compress.h"
#include "resource.h"

#ifdef _MSC_VER
#include <delayimp.h>
#pragma comment(lib, "avcodec-57")
#pragma comment(lib, "avformat-57")
#pragma comment(lib, "avutil-55")
#pragma comment(lib, "swscale-4")
#pragma comment(lib, "swresample-2")
#pragma comment(lib, "delayimp")
#endif

HINSTANCE hInstance;
bool config_decode_raw = false;
bool config_decode_magic = false;
bool config_decode_cfhd = false;
void saveConfig();

int av_initialized;

void av_log_func(void* obj, int type, const char* msg, va_list arg)
{
  char buf[1024];
  vsprintf(buf,msg,arg);
  OutputDebugString(buf);
  switch(type){
  case AV_LOG_PANIC:
  case AV_LOG_FATAL:
  case AV_LOG_ERROR:
  case AV_LOG_WARNING:
    ;//DebugBreak();
  }
}

void init_av()
{
  if(!av_initialized){
    av_initialized = 1;
    av_register_all();
    avcodec_register_all();
    //av_register_cfhd();
    av_register_vfw_cfhd();

    #ifdef FFDEBUG
    //av_log_set_callback(av_log_func);
    //av_log_set_level(AV_LOG_INFO);
    //av_log_set_flags(AV_LOG_SKIP_REPEATED);
    #endif
  }
}

class ConfigureDialog: public VDXVideoFilterDialog {
public:
	virtual INT_PTR DlgProc(UINT msg, WPARAM wParam, LPARAM lParam);
  void Show(HWND parent){
    VDXVideoFilterDialog::Show(hInstance,MAKEINTRESOURCE(IDD_INPUT_OPTIONS),parent);
  }
};

INT_PTR ConfigureDialog::DlgProc(UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch( msg ){
  case WM_INITDIALOG:
    CheckDlgButton(mhdlg,IDC_DECODE_RAW, config_decode_raw ? BST_CHECKED:BST_UNCHECKED);
    CheckDlgButton(mhdlg,IDC_DECODE_MAGIC, config_decode_magic ? BST_CHECKED:BST_UNCHECKED);
    CheckDlgButton(mhdlg,IDC_DECODE_CFHD, config_decode_cfhd ? BST_CHECKED:BST_UNCHECKED);
    return TRUE;
  case WM_COMMAND:
    switch(LOWORD(wParam)){
    case IDOK:
      config_decode_raw = IsDlgButtonChecked(mhdlg,IDC_DECODE_RAW)!=0;
      config_decode_magic = IsDlgButtonChecked(mhdlg,IDC_DECODE_MAGIC)!=0;
      config_decode_cfhd = IsDlgButtonChecked(mhdlg,IDC_DECODE_CFHD)!=0;
      saveConfig();
      EndDialog(mhdlg, TRUE);
      return TRUE;

    case IDCANCEL:
      EndDialog(mhdlg, FALSE);
      return TRUE;
    }
  }
  return false;
}

bool VDXAPIENTRY StaticConfigureProc(VDXHWND parent)
{
  ConfigureDialog dlg;
  dlg.Show((HWND)parent);
  return true;
}

///////////////////////////////////////////////////////////////////////////////

bool VDXAPIENTRY ff_create_a(const VDXInputDriverContext *pContext, IVDXInputFileDriver **ppDriver)
{
  VDFFInputFileDriver *p = new VDFFInputFileDriver(*pContext);
  if(!p) return false;
  p->select_mode = true;
  *ppDriver = p;
  p->AddRef();
  return true;
}

bool VDXAPIENTRY ff_create_b(const VDXInputDriverContext *pContext, IVDXInputFileDriver **ppDriver)
{
  VDFFInputFileDriver *p = new VDFFInputFileDriver(*pContext);
  if(!p) return false;
  *ppDriver = p;
  p->AddRef();
  return true;
}

#define option_a_init L"FFMpeg (select formats)|*.mov;*.mp4;*.avi"
std::wstring option_a = option_a_init;
std::wstring pattern_a; // example "*.mov|*.mp4|*.avi"

VDXInputDriverDefinition ff_class_a={
  sizeof(VDXInputDriverDefinition),
  VDXInputDriverDefinition::kFlagSupportsVideo|
  VDXInputDriverDefinition::kFlagSupportsAudio|
  VDXInputDriverDefinition::kFlagCustomSignature|
  VDXInputDriverDefinition::kFlagForceByName|
  VDXInputDriverDefinition::kFlagNoOptions,
  1, //priority, reset from options
  0, //SignatureLength
  0, //Signature
  pattern_a.c_str(),
  option_a.c_str(),
  L"ffmpeg_select",
  ff_create_a
};

#define option_b_init L"FFMpeg (all formats)|*.mov;*.mp4;*.avi"
std::wstring option_b = option_b_init;

VDXInputDriverDefinition ff_class_b={
  sizeof(VDXInputDriverDefinition),
  VDXInputDriverDefinition::kFlagSupportsVideo|
  VDXInputDriverDefinition::kFlagSupportsAudio|
  VDXInputDriverDefinition::kFlagNoOptions,
  -2, //priority, reset from options
  0, //SignatureLength
  0, //Signature
  0,
  option_b.c_str(),
  L"ffmpeg_default",
  ff_create_b
};

VDXPluginInfo ff_plugin_b={
  sizeof(VDXPluginInfo),
  L"FFMpeg (all formats)",
  L"Anton Shekhovtsov",
  L"Loads and decode files through ffmpeg libs.",
  (1 << 24) + (9 << 16),
  kVDXPluginType_Input,
  0,
  10, // min api version
  kVDXPlugin_APIVersion,
  4,  // min input api version
  kVDXPlugin_InputDriverAPIVersion,
  &ff_class_b
};

VDXPluginInfo ff_plugin_a;
VDPluginInfo* kPlugins[]={
  &ff_output_info,
  &ff_mp3enc_info,
  &ff_aacenc_info,
  0,0, // reserved for input drivers
  0
};

extern "C" VDPluginInfo** __cdecl VDGetPluginInfo()
{
  return kPlugins;
}

void saveConfig()
{
  wchar_t buf[MAX_PATH+128];
  size_t n = GetModuleFileNameW(hInstance, buf, MAX_PATH);
  if(n<=0) return;
  buf[n] = 0;

  wchar_t* p1 = wcsrchr(buf,'\\');
  wchar_t* p2 = wcsrchr(buf,'/');
  if(p2>p1) p1=p2;
  if(!p1) return;
  *p1 = 0;

  wcscat(buf,L"\\cch_input.ini");

  WritePrivateProfileStringW(L"force_ffmpeg",L"raw",config_decode_raw ? L"1":L"0",buf);
  WritePrivateProfileStringW(L"force_ffmpeg",L"MagicYUV",config_decode_magic ? L"1":L"0",buf);
  WritePrivateProfileStringW(L"force_ffmpeg",L"CineformHD",config_decode_cfhd ? L"1":L"0",buf);
  WritePrivateProfileStringW(0,0,0,buf);
}

void loadConfig()
{
  wchar_t buf[MAX_PATH+128];
  size_t n = GetModuleFileNameW(hInstance, buf, MAX_PATH);
  if(n<=0) return;
  buf[n] = 0;

  wchar_t* p1 = wcsrchr(buf,'\\');
  wchar_t* p2 = wcsrchr(buf,'/');
  if(p2>p1) p1=p2;
  if(!p1) return;
  *p1 = 0;

  wcscat(buf,L"\\cch_input.ini");

  config_decode_raw = GetPrivateProfileIntW(L"force_ffmpeg",L"raw",0,buf)!=0;
  config_decode_magic = GetPrivateProfileIntW(L"force_ffmpeg",L"MagicYUV",0,buf)!=0;
  config_decode_cfhd = GetPrivateProfileIntW(L"force_ffmpeg",L"CineformHD",0,buf)!=0;

  ff_plugin_a = ff_plugin_b;
  ff_plugin_a.mpTypeSpecificInfo = &ff_class_a;
  ff_plugin_a.mpName = L"FFMpeg (select formats)"; // name must be unique
  ff_plugin_a.mpStaticConfigureProc = StaticConfigureProc;

  int priority_a = GetPrivateProfileIntW(L"priority",L"select",ff_class_a.mPriority,buf);
  int priority_b = GetPrivateProfileIntW(L"priority",L"default",ff_class_b.mPriority,buf);

  {
    option_a = option_a_init;
    int mp = option_a.rfind('|');
    wchar_t mask[2048];
    GetPrivateProfileStringW(L"file_mask",L"select",&option_a[mp+1],mask,2048,buf);
    option_a.resize(mp+1);

    int p = 0;
    int n = wcslen(mask);
    pattern_a = L"";
    int count1 = 0;
    int count2 = 0;
    while(p<n){
      int p1 = n;
      wchar_t* p2 = wcschr(mask+p,';');
      if(p2 && p2-mask<p1) p1 = p2-mask;

      bool skip = false;
      if(wcsncmp(mask+p,L"*.avi",5)==0) skip = true;
      if(wcsncmp(mask+p,L"*.mp4",5)==0) skip = true;
      if(wcsncmp(mask+p,L"*.mov",5)==0) skip = true;

      if(count1) option_a += L";";
      option_a += std::wstring(mask+p,p1-p);

      if(!skip){
        if(count2) pattern_a += L"|";
        pattern_a += std::wstring(mask+p,p1-p);
      }
      p = p1+1;
      count1++;
      count2++;
    }

    ff_class_a.mpFilenameDetectPattern = pattern_a.c_str();
    ff_class_a.mpFilenamePattern = option_a.c_str();
  }

  {
    option_b = option_b_init;
    int mp = option_b.rfind('|');
    wchar_t mask[2048];
    GetPrivateProfileStringW(L"file_mask",L"default",&option_b[mp+1],mask,2048,buf);
    option_b.resize(mp+1);

    int p = 0;
    int n = wcslen(mask);
    int count1 = 0;
    while(p<n){
      int p1 = n;
      wchar_t* p2 = wcschr(mask+p,';');
      if(p2 && p2-mask<p1) p1 = p2-mask;

      if(count1) option_b += L";";
      option_b += std::wstring(mask+p,p1-p);

      p = p1+1;
      count1++;
    }

    ff_class_b.mpFilenamePattern = option_b.c_str();
  }

  int pos = 0;
  while(kPlugins[pos]) pos++;

  if(priority_a==priority_b){
    ff_class_b.mPriority = priority_b;
    kPlugins[pos] = &ff_plugin_b;
  } else {
    ff_class_a.mPriority = priority_a;
    ff_class_b.mPriority = priority_b;
    kPlugins[pos] = &ff_plugin_a;
    kPlugins[pos+1] = &ff_plugin_b;
  }
}

#ifdef _MSC_VER

HINSTANCE module_base[5];

bool loadModules()
{
  wchar_t buf[MAX_PATH+128];
  size_t n = GetModuleFileNameW(hInstance, buf, MAX_PATH);
  if(n<=0) return false;
  buf[n] = 0;

  wchar_t* p1 = wcsrchr(buf,'\\');
  wchar_t* p2 = wcsrchr(buf,'/');
  if(p2>p1) p1=p2;
  if(!p1) return false;

  *p1 = 0;
  wcscat(buf,L"\\ffdlls\\");
  p1+=8;

  wchar_t* module_name[5] = {
    L"avutil-55.dll",
    L"swresample-2.dll",
    L"swscale-4.dll",
    L"avcodec-57.dll",
    L"avformat-57.dll",
  };

  {for(int i=0; i<5; i++){
    *p1 = 0;
    wcscat(buf,module_name[i]);
    HINSTANCE h = LoadLibraryW(buf);
    if(!h){
      wchar_t buf2[MAX_PATH+128];
      wcscpy(buf2,L"Module failed to load correctly:\n");
      wcscat(buf2,buf);
      MessageBoxW(0,buf2,L"FFMpeg input driver",MB_OK|MB_ICONSTOP);
      return false;
    }
    module_base[i] = h;
  }}

  return true;
}

void unloadModules()
{
  char* module_name[6] = {
    "avutil-55.dll",
    "swresample-2.dll",
    "swscale-4.dll",
    "avcodec-57.dll",
    "avformat-57.dll",
    #ifdef _WIN64
    "cfhddecoder64.dll",
    #else
    "cfhddecoder.dll",
    #endif
  };

  {for(int i=5; i>=0; i--){
    __FUnloadDelayLoadedDLL2(module_name[i]);
  }}

  {for(int i=4; i>=0; i--){
    HINSTANCE h = module_base[i];
    if(h) FreeLibrary(h);
  }}
}

BOOLEAN WINAPI DllMain( IN HINSTANCE hDllHandle, IN DWORD nReason, IN LPVOID Reserved )
{
  switch ( nReason ){
  case DLL_PROCESS_ATTACH:
    hInstance = hDllHandle;
    if(!loadModules()) return false;
    loadConfig();
    return true;

  case DLL_PROCESS_DETACH:
    if(Reserved==0) unloadModules();
    return true;
  }

  return true;
}

#endif

#ifdef __GNUC__

BOOLEAN WINAPI DllMain( IN HINSTANCE hDllHandle, IN DWORD nReason, IN LPVOID Reserved )
{
  switch ( nReason ){
  case DLL_PROCESS_ATTACH:
    hInstance = hDllHandle;
    loadConfig();
    return true;

  case DLL_PROCESS_DETACH:
    return true;
  }

  return true;
}

#endif
