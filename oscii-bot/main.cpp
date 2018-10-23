// OSCII-bot
// Copyright (C) 2013 Cockos Incorporated
// License: GPL

#ifndef __APPLE__
#include "../WDL/swell/swell.h"
#include "../WDL/swell/swell-internal.h"


int main(int argc, char **argv)
{
  extern char **g_argv;
  extern int g_argc;
  extern HWND g_hwnd;

  g_argc=argc;
  g_argv=argv;
  SWELL_initargs(&argc,&argv);
  SWELL_Internal_PostMessage_Init();
  SWELL_ExtendedAPI("APPNAME",(void*)"OSCII-bot");
  SWELLAppMain(SWELLAPP_ONLOAD,0,0);
  SWELLAppMain(SWELLAPP_LOADED,0,0);
  while (g_hwnd && !g_hwnd->m_hashaddestroy) {
    SWELL_RunMessageLoop();
    Sleep(10);
  }
  SWELLAppMain(SWELLAPP_DESTROY,0,0);
  return 0;
}
#endif
