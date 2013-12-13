#ifndef _M2O_DEVICE_H_
#define _M2O_DEVICE_H_


#include "../WDL/wdlstring.h"
#include "../WDL/ptrlist.h"

class outputDevice {
  protected:
    outputDevice() { }
  public:
    virtual ~outputDevice() { }
    virtual void run()=0;
    virtual void oscSend(const char *src, int len) { }
    virtual void midiSend(const unsigned char *buf, int len) { }
    virtual const char *get_type()=0;
};

class inputDevice {
  protected:
    inputDevice() { }
    struct rec
    {
      void (*callback)(void *d1, void *d2, char type, int msglen, void *msg); // type=0 for MIDI, 1=osc
      void *data1;
      void *data2;
    };
    WDL_TypedBuf<rec> m_instances;

  public:
    virtual ~inputDevice() { };
    virtual void start()=0;
    virtual void run(WDL_FastString &textOut)=0;
    virtual const char *get_type()=0;

    void addinst(void (*callback)(void *d1, void *d2, char type, int msglen, void *msg), void *d1, void *d2)
    {
      const rec r={callback,d1,d2};
      m_instances.Add(r);
    }
};



class midiOutputDevice : public outputDevice
{
public:
  midiOutputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<outputDevice> *reuseDevList);

  virtual ~midiOutputDevice();

  virtual void run();
  virtual void midiSend(const unsigned char *buf, int len);
  virtual const char *get_type() { return "MIDI"; }

  char *m_name_substr,*m_name_used;
  int m_skipcnt;

  midiOutputDevice *m_open_would_use_altdev; // set during constructor if device already referenced in reuseDevList
  int m_last_dev_idx;

#ifdef _WIN32
  DWORD m_failed_time;
  HMIDIOUT m_handle;
  static void CALLBACK callbackFunc(
    HMIDIOUT hMidiIn,  
    UINT wMsg,        
    LPARAM dwInstance, 
    LPARAM dwParam1,   
    LPARAM dwParam2    
    ) { }
#endif

#ifdef __APPLE__
  void *m_handle;
  time_t m_failed_time;
#endif

  void do_open(WDL_PtrList<outputDevice> *reuseDevList=NULL);
  void do_close();

};


class midiInputDevice : public inputDevice
{
public:
  midiInputDevice(const char *namesubstr, int skipcnt, WDL_PtrList<inputDevice> *reuseDevList);

  virtual ~midiInputDevice();
  virtual const char *get_type() { return "MIDI"; }

  virtual void start();
  virtual void run(WDL_FastString &textOut);

#ifdef _WIN32
  static void CALLBACK callbackFunc(
    HMIDIIN hMidiIn,  
    UINT wMsg,        
    LPARAM dwInstance, 
    LPARAM dwParam1,   
    LPARAM dwParam2    
  );


  HMIDIIN m_handle;
  WDL_PtrList<MIDIHDR> m_longmsgs;
  DWORD m_lastmsgtime;
#endif

#ifdef __APPLE__
  void *m_handle;
  time_t m_lastmsgtime;
#endif

  char *m_name_substr,*m_name_used;
  int m_input_skipcnt;

  midiInputDevice *m_open_would_use_altdev; // set during constructor if device already referenced in reuseDevList
  int m_last_dev_idx;

  void do_open(WDL_PtrList<inputDevice> *reuseDevList=NULL);
  void do_close();

};


#endif
