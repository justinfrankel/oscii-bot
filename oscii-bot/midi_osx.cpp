#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
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
  m_lastmsgtime = time(NULL);

  int x;
  const int n=0;// midiInGetNumDevs();
  int skipcnt = m_input_skipcnt;
  for(x=0;x<n;x++)
  {
    //MIDIINCAPS caps;
    if (0) //midiInGetDevCaps(x,&caps,sizeof(caps)) == MMSYSERR_NOERROR)
    {
//      if ((!m_name_substr[0] || strstr(caps.szPname,m_name_substr)) && !skipcnt--)
      {
        free(m_name_used);
//  /      m_name_used = strdup(caps.szPname);

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
        if (0) //midiInOpen(&m_handle,x,(LPARAM)callbackFunc,(LPARAM)this,CALLBACK_FUNCTION) != MMSYSERR_NOERROR )
        {
          m_handle=0;
        }
        else
        break;
      }
    }
  }
}


void midiInputDevice::do_close()
{
  if (m_handle) 
  {
//    midiInReset(m_handle);
//    midiInClose(m_handle); 
    m_handle=0;
  }
}


void midiInputDevice::start() 
{ 
  if (m_handle) 
  {
 //   midiInStop(m_handle);
//    midiInStart(m_handle); 
  }
}
void midiInputDevice::run(WDL_FastString &textOut)
{
  int x;

  const time_t now=time(NULL);
  if (now > m_lastmsgtime+5) // every 5s of inactivity, query status
  {
    m_lastmsgtime = now;
//    MIDIINCAPS caps;
    if (0) //!m_handle || midiInGetDevCaps((UINT)m_handle,&caps,sizeof(caps))!=MMSYSERR_NOERROR)
    {
      const bool had_handle=!!m_handle;
      do_close();
      do_open();
      if (m_handle) 
      {
        textOut.AppendFormatted(1024,"Reopened device %s\r\n",m_name_used);
        start();
      }
      else if (had_handle) textOut.AppendFormatted(1024,"Closed device %s\r\n",m_name_used);
    }
  }
}

/*
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

      int x;
      const int n=_this->m_instances.GetSize();
      const rec *r = _this->m_instances.Get();
      for (x=0;x<n; x++)
        if (r[x].callback) r[x].callback(r[x].data1,r[x].data2,0,3,(void*)msg);
    }
  }
}
*/





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
  const int n=0;//midiOutGetNumDevs();
  int skipcnt = m_skipcnt;
  for(x=0;x<n;x++)
  {
//    MIDIOUTCAPS caps;
    if (0) //midiOutGetDevCaps(x,&caps,sizeof(caps)) == MMSYSERR_NOERROR)
    {
     // if ((!m_name_substr[0] || strstr(caps.szPname,m_name_substr)) && !skipcnt--)
      {
        free(m_name_used);
       // m_name_used = strdup(caps.szPname);

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
        if (0) //midiOutOpen(&m_handle,x,(LPARAM)callbackFunc,(LPARAM)this,CALLBACK_FUNCTION) != MMSYSERR_NOERROR )
        {
          m_handle=0;
          m_failed_time=time(NULL);
        }
        break;
      }
    }
  }
}


void midiOutputDevice::do_close()
{
  if (m_handle) 
  {
//    midiOutReset(m_handle);

 //   midiOutClose(m_handle); 
    m_handle=0;
  }
}


void midiOutputDevice::run()
{
  if (m_failed_time && time(NULL)>m_failed_time+1)
  {
    // try to reopen
    m_failed_time=0;
    do_open();
  }
}

void midiOutputDevice::midiSend(const unsigned char *buf, int len)
{
  if (m_handle && len>0 && len <= 3)
  {
    int a = buf[0];
    if (len>=2) a|=(((int)buf[1])<<8);
    if (len>=3) a|=(((int)buf[2])<<16);
  //  if (midiOutShortMsg(m_handle,a) != MMSYSERR_NOERROR && !m_failed_time)
   //   m_failed_time=GetTickCount();
  }
}
