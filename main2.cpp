#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vd2/plugin/vdplugin.h>
#include <vd2/plugin/vdinputdriver.h>
#include "InputFile2.h"
#include "version2.h"
#include <delayimp.h>

#ifndef __GNUC__
#pragma comment(lib, "avcodec-57")
#pragma comment(lib, "avformat-57")
#pragma comment(lib, "avutil-55")
#pragma comment(lib, "swscale-4")
#pragma comment(lib, "swresample-2")
#pragma comment(lib, "delayimp")
#endif

HINSTANCE hInstance;

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

wchar_t option_a[1024] = L"FFMpeg (select formats)|*.mov;*.mp4;*.avi";
wchar_t pattern_a[1024] = L""; // example "*.mov|*.mp4|*.avi"

VDXInputDriverDefinition ff_class_a={
  sizeof(VDXInputDriverDefinition),
  VDXInputDriverDefinition::kFlagSupportsVideo|VDXInputDriverDefinition::kFlagSupportsAudio|VDXInputDriverDefinition::kFlagCustomSignature|VDXInputDriverDefinition::kFlagForceByName|VDXInputDriverDefinition::kFlagNoOptions,
  1, //priority, reset from options
  0, //SignatureLength
  0, //Signature
  pattern_a,
  option_a,
  L"ffmpeg_select",
  ff_create_a
};

wchar_t option_b[1024] = L"FFMpeg (all formats)|*.mov;*.mp4;*.avi";

VDXInputDriverDefinition ff_class_b={
  sizeof(VDXInputDriverDefinition),
  VDXInputDriverDefinition::kFlagSupportsVideo|VDXInputDriverDefinition::kFlagSupportsAudio|VDXInputDriverDefinition::kFlagNoOptions,
  -2, //priority, reset from options
  0, //SignatureLength
  0, //Signature
  0,
  option_b,
  L"ffmpeg_default",
  ff_create_b
};

VDXPluginInfo ff_plugin_b={
  sizeof(VDXPluginInfo),
  L"FFMpeg (all formats)",
  L"Anton Shekhovtsov",
  L"Loads and decode files through ffmpeg libs.",
  (FFDRIVER_VERSION_MAJOR << 24) + (FFDRIVER_VERSION_MINOR << 16) + (FFDRIVER_VERSION_REVISION << 8) + FFDRIVER_VERSION_BUILD,
  kVDXPluginType_Input,
  0,
  kVDXPlugin_APIVersion,										// 10
  kVDXPlugin_APIVersion,                    // 10
  kVDXPlugin_InputDriverAPIVersion,         //  2
  kVDXPlugin_InputDriverAPIVersion,         //  2
  &ff_class_b
};

VDXPluginInfo ff_plugin_a;
VDPluginInfo* kPlugins[]={0,0,0};

extern "C" __declspec(dllexport) VDPluginInfo** __cdecl VDGetPluginInfo()
{
  return kPlugins;
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

  ff_plugin_a = ff_plugin_b;
  ff_plugin_a.mpTypeSpecificInfo = &ff_class_a;
  ff_plugin_a.mpName = L"FFMpeg (select formats)"; // name must be unique

  int priority_a = GetPrivateProfileIntW(L"priority",L"select",ff_class_a.mPriority,buf);
  int priority_b = GetPrivateProfileIntW(L"priority",L"default",ff_class_b.mPriority,buf);

  wchar_t* mp = wcsrchr(option_a,'|');
  if(mp){
    wchar_t mask[1024];
    GetPrivateProfileStringW(L"file_mask",L"select",mp+1,mask,1024,buf);
    mp[1] = 0;
    wcscat_s(option_a,1024,mask);

    int p = 0;
    int n = wcslen(mask);
    pattern_a[0] = 0;
    while(p<n){
      int p1 = n;
      wchar_t* p2 = wcschr(mask+p,';');
      if(p2 && p2-mask<p1) p1 = p2-mask;

      bool skip = false;
      if(wcsncmp(mask+p,L"*.avi",5)==0) skip = true;
      if(wcsncmp(mask+p,L"*.mp4",5)==0) skip = true;
      if(wcsncmp(mask+p,L"*.mov",5)==0) skip = true;

      if(!skip){
        if(pattern_a[0]!=0) wcscat_s(pattern_a,1024,L"|");
        wcsncat_s(pattern_a,1024,mask+p,p1-p);
      }
      p = p1+1;
    }
  }
  mp = wcsrchr(option_b,'|');
  if(mp){
    wchar_t mask[1024];
    GetPrivateProfileStringW(L"file_mask",L"default",mp+1,mask,1024,buf);
    mp[1] = 0;
    wcscat_s(option_b,1024,mask);
  }

  if(priority_a==priority_b){
    ff_class_b.mPriority = priority_b;
    kPlugins[0] = &ff_plugin_b;
  } else {
    ff_class_a.mPriority = priority_a;
    ff_class_b.mPriority = priority_b;
    kPlugins[0] = &ff_plugin_a;
    kPlugins[1] = &ff_plugin_b;
  }
}

#ifndef __GNUC__

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
