// OSCII-bot
// Copyright (C) 2013 Cockos Incorporated
// License: GPL

#include <windows.h>
#include <ctype.h>
#include <math.h>

#include "device.h"

midiInputDevice::midiInputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<inputDevice> *reuseDevList) 
{
  // found device!
  m_name_substr = strdup(namesubstr);
  m_input_skipcnt=skipcnt;
  m_handle=0;
  m_name_used=0;
  m_open_would_use_altdev = NULL;
  m_last_dev_idx=-1;
  
  do_open(reuseDevList);
}

midiInputDevice::~midiInputDevice() 
{
  do_close();
  free(m_name_substr);
  free(m_name_used);
}

void midiInputDevice::do_open(WDL_PtrList<inputDevice> *reuseDevList)
{
  do_close();
  m_lastmsgtime = GetTickCount();

  int x;
  const int n=midiInGetNumDevs();
  int skipcnt = m_input_skipcnt;
  for(x=0;x<n;x++)
  {
    MIDIINCAPS caps;
    if (midiInGetDevCaps(x,&caps,sizeof(caps)) == MMSYSERR_NOERROR)
    {
      if ((!m_name_substr[0] || strstr(caps.szPname,m_name_substr)) && !skipcnt--)
      {
        free(m_name_used);
        m_name_used = strdup(caps.szPname);

        if (reuseDevList)
        {
          int i;
          for (i=0;i<reuseDevList->GetSize();i++)
          {
            inputDevice *dev=reuseDevList->Get(i);
            if (dev && !strcmp(dev->get_type(),"MIDI"))
            {
              midiInputDevice *mid = (midiInputDevice *)dev;
              if (mid->m_last_dev_idx == x)
              {
                m_open_would_use_altdev = mid;
                return;
              }
            }
          }
        }

        m_last_dev_idx = x;
        if (midiInOpen(&m_handle,x,(LPARAM)callbackFunc,(LPARAM)this,CALLBACK_FUNCTION) != MMSYSERR_NOERROR )
        {
          m_handle=0;
        }
        else
        {
          int x;
          for (x = 0; x < 8; x ++)
          {
            const int bufsz=1024;
            MIDIHDR *hdr=(MIDIHDR *)malloc(sizeof(MIDIHDR)+bufsz);
            memset(hdr,0,sizeof(MIDIHDR));
            hdr->dwBufferLength = bufsz;
            hdr->lpData = (char *)(hdr+1);
            m_longmsgs.Add(hdr);
          }
        }
        break;
      }
    }
  }
}


void midiInputDevice::do_close()
{
  if (m_handle) 
  {
    midiInReset(m_handle);
    int x;
    int n=500;
    Sleep(100);
    for (x = 0; x < m_longmsgs.GetSize(); x ++)
    {
      MIDIHDR *hdr=m_longmsgs.Get(x);
      while (!(hdr->dwFlags & MHDR_DONE) && (hdr->dwFlags & MHDR_INQUEUE) && n-->0)
        Sleep(10);
    }
    for (x = 0; x < m_longmsgs.GetSize(); x ++)
    {
      MIDIHDR *hdr=m_longmsgs.Get(x);
      midiInUnprepareHeader(m_handle,hdr,sizeof(MIDIHDR));
    }
    m_longmsgs.Empty(true,free);

    midiInClose(m_handle); 
    m_handle=0;
  }
}


void midiInputDevice::start() 
{ 
  if (m_handle) 
  {
    midiInStop(m_handle);
    int x;
    for (x = 0; x < m_longmsgs.GetSize(); x ++)
    {
      if (!(m_longmsgs.Get(x)->dwFlags & MHDR_INQUEUE) || (m_longmsgs.Get(x)->dwFlags & MHDR_DONE))
      {
        midiInUnprepareHeader(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
        m_longmsgs.Get(x)->dwFlags=0;
        m_longmsgs.Get(x)->dwBytesRecorded=0;
        midiInPrepareHeader(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
        midiInAddBuffer(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
      }
    }

    midiInStart(m_handle); 
  }
}
void midiInputDevice::run(WDL_FastString &textOut)
{
  int x;

  if (m_handle) for (x = 0; x < m_longmsgs.GetSize(); x ++)
  {
    MIDIHDR *hdr=m_longmsgs.Get(x);
    if (hdr->dwFlags & MHDR_DONE)
    {
      midiInUnprepareHeader(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
      m_longmsgs.Get(x)->dwFlags=0;
      m_longmsgs.Get(x)->dwBytesRecorded=0;
    }
    if (!(hdr->dwFlags & MHDR_PREPARED))
    {
      midiInPrepareHeader(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
    }
    if (!(hdr->dwFlags & MHDR_INQUEUE))
    {
      midiInAddBuffer(m_handle,m_longmsgs.Get(x),sizeof(MIDIHDR));
    }
  }

  const DWORD now=GetTickCount();
  if (now > m_lastmsgtime+5000) // every 5s of inactivity, query status
  {
    m_lastmsgtime = GetTickCount();
    MIDIINCAPS caps;
    if (!m_handle || midiInGetDevCaps((UINT)m_handle,&caps,sizeof(caps))!=MMSYSERR_NOERROR)
    {
      const bool had_handle=!!m_handle;
      do_close();
      do_open();
      if (m_handle) 
      {
        textOut.AppendFormatted(1024,"***** Reopened MIDI input device %s\r\n",m_name_used);
        start();
      }
      else if (had_handle) textOut.AppendFormatted(1024,"***** Lost MIDI input device %s\r\n",m_name_used);
    }
  }
}

void CALLBACK midiInputDevice::callbackFunc(
  HMIDIIN hMidiIn,  
  UINT wMsg,        
  LPARAM dwInstance, 
  LPARAM dwParam1,   
  LPARAM dwParam2    
)
{
  midiInputDevice *_this = (midiInputDevice*)dwInstance;
  if (wMsg == MIM_DATA )
  {
    if (_this) 
    {
      const unsigned char msg[3] = {
        (unsigned char)(dwParam1&0xff),
        (unsigned char)((dwParam1>>8)&0xff),
        (unsigned char)((dwParam1>>16)&0xff)
      };

      _this->m_lastmsgtime = GetTickCount();

      _this->onMessage(0,msg,3);
    }
  }
}



midiOutputDevice::midiOutputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<outputDevice> *reuseDevList) 
{
  // found device!
  m_name_substr = strdup(namesubstr);
  m_skipcnt=skipcnt;
  m_handle=0;
  m_name_used=0;
  m_open_would_use_altdev = NULL;
  m_last_dev_idx=-1;
  
  do_open(reuseDevList);
}

midiOutputDevice::~midiOutputDevice() 
{
  do_close();
  free(m_name_substr);
  free(m_name_used);
}

void midiOutputDevice::do_open(WDL_PtrList<outputDevice> *reuseDevList)
{
  do_close();
  m_failed_time=0;

  int x;
  const int n=midiOutGetNumDevs();
  int skipcnt = m_skipcnt;
  for(x=0;x<n;x++)
  {
    MIDIOUTCAPS caps;
    if (midiOutGetDevCaps(x,&caps,sizeof(caps)) == MMSYSERR_NOERROR)
    {
      if ((!m_name_substr[0] || strstr(caps.szPname,m_name_substr)) && !skipcnt--)
      {
        free(m_name_used);
        m_name_used = strdup(caps.szPname);

        if (reuseDevList)
        {
          int i;
          for (i=0;i<reuseDevList->GetSize();i++)
          {
            outputDevice *dev=reuseDevList->Get(i);
            if (dev && !strcmp(dev->get_type(),"MIDI"))
            {
              midiOutputDevice *mid = (midiOutputDevice *)dev;
              if (mid->m_last_dev_idx == x)
              {
                m_open_would_use_altdev = mid;
                return;
              }
            }
          }
        }

        m_last_dev_idx = x;
        if (midiOutOpen(&m_handle,x,(LPARAM)callbackFunc,(LPARAM)this,CALLBACK_FUNCTION) != MMSYSERR_NOERROR )
        {
          m_handle=0;
        }
        break;
      }
    }
  }
  if (!m_handle)
    m_failed_time=GetTickCount();


}


void midiOutputDevice::do_close()
{
  if (m_handle) 
  {
    midiOutReset(m_handle);

    midiOutClose(m_handle); 
    m_handle=0;
  }
}


void midiOutputDevice::run(WDL_FastString &textOut)
{
  if (m_failed_time && GetTickCount()>m_failed_time+1000)
  {
    // try to reopen
    const bool had_handle = !!m_handle;
    do_open();

    if (m_handle) textOut.AppendFormatted(1024,"***** Reopened MIDI output device %s\r\n",m_name_used);
    else if (had_handle) textOut.AppendFormatted(1024,"***** Lost MIDI output device %s\r\n",m_name_used);

  }
}

void midiOutputDevice::midiSend(const unsigned char *buf, int len)
{
  if (m_handle && len>0 && len <= 3)
  {
    int a = buf[0];
    if (len>=2) a|=(((int)buf[1])<<8);
    if (len>=3) a|=(((int)buf[2])<<16);
    if (midiOutShortMsg(m_handle,a) != MMSYSERR_NOERROR && !m_failed_time)
      m_failed_time=GetTickCount();
  }
}
