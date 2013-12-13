// OSCII v0.2
// Copyright (C) 2013 Cockos Incorporated
// License: GPL
#ifdef _WIN32
#include <windows.h>
#else
#include "../WDL/swell/swell.h"
#endif

#include <ctype.h>
#include <math.h>

#include "resource.h"
#include "../WDL/eel2/ns-eel-int.h"
#include "../WDL/wdlcstring.h"
#include "../WDL/dirscan.h"
#include "../WDL/queue.h"
#include "../WDL/mutex.h"
#include "../WDL/lineparse.h"
#include "../WDL/wingui/wndsize.h"

#include "device.h"
#include "oscmsg.h"

#if defined(_MSC_VER) && defined(strcasecmp)
#undef strcasecmp
#endif

#include "../WDL/jnetlib/jnetlib.h"

HINSTANCE g_hInstance;

#define WM_SYSTRAY              WM_USER + 0x200
BOOL systray_add(HWND hwnd, UINT uID, HICON hIcon, LPSTR lpszTip);
BOOL systray_del(HWND hwnd, UINT uID);


int g_recent_events[4];
const char *g_code_names[4] = { "@init", "@timer", "@midimsg", "@oscmsg" };

HWND g_hwnd;
RECT g_last_wndpos;
WDL_FastString g_ini_file;
WDL_FastString g_default_script_path;
WDL_PtrList<char> g_script_load_filenames, g_script_load_paths;

class scriptInstance 
{
  public:
    scriptInstance(const char *fn, WDL_FastString &results) 
    { 
      m_debugOut=0;
      m_fn.Set(fn);
      m_vm=0;
      m_cur_oscmsg=0;
      memset(m_code,0,sizeof(m_code));
      clear();
      load_script(results);
    }
    ~scriptInstance() 
    {
      clear();
    }
    void load_script(WDL_FastString &results);
    void clear()
    {
      m_debugOut=0;
      m_in_devs.Empty();
      m_out_devs.Empty();
      m_strings.Empty(true,free);
      int x;
      for (x=0;x<sizeof(m_code)/sizeof(m_code[0]); x++) 
      {
        if (m_code[x]) NSEEL_code_free(m_code[x]);
        m_code[x]=0;
      }
      if (m_vm) NSEEL_VM_free(m_vm);
      m_vm=0;
      m_incoming_events.Resize(0,false);

      m_var_time = 0;
      memset(m_var_msgs,0,sizeof(m_var_msgs));
      memset(m_var_fmt,0,sizeof(m_var_fmt));
    }

    void start(WDL_FastString &results)
    {
      if (m_code[0])
      {
        if (m_var_time) *m_var_time = timeGetTime()/1000.0;
        m_debugOut = &results;
        NSEEL_code_execute(m_code[0]);
        m_debugOut = NULL;
      }
    }

    void compileCode(int parsestate, const WDL_FastString &curblock, WDL_FastString &results, int lineoffs);
    bool run(double curtime, WDL_FastString &results);
    static void messageCallback(void *d1, void *d2, char type, int msglen, void *msg);

    WDL_String m_fn;
    WDL_FastString *m_debugOut;

    struct incomingEvent
    {
      EEL_F *dev_ptr;
      int sz; // size of msg[], 1..3 for midi, anything for OSC
      char type; // 0=midi, 1=OSC
      unsigned char msg[3];
    };


    // these are non-owned refs
    WDL_PtrList<inputDevice> m_in_devs;
    WDL_PtrList<outputDevice> m_out_devs;

    WDL_HeapBuf m_incoming_events;  // incomingEvent list, each is 8-byte aligned
    WDL_Mutex m_incoming_events_mutex;

    WDL_PtrList<char> m_strings;

    enum { MAX_OSC_FMTS=32 };

    EEL_F *m_var_time, *m_var_msgs[4], *m_var_fmt[MAX_OSC_FMTS];
    NSEEL_VMCTX m_vm;
    NSEEL_CODEHANDLE m_code[4]; // init, timer, message code, oscmsg code

    const OscMessageRead *m_cur_oscmsg;
  

    enum {
        STRING_INDEX_BASE=0x100000,
        INPUT_INDEX_BASE =0x400000,
        OUTPUT_INDEX_BASE=0x500000
    };

    bool format_strings(const char *fmt, char *buf, int buf_sz, int want_escapes, const WDL_PtrList<char> *stringTab);
    static EEL_F NSEEL_CGEN_CALL _send_oscevent(void *opaque, EEL_F *dest_device, EEL_F *fmt_index, EEL_F *value);
    static EEL_F NSEEL_CGEN_CALL _send_midievent(void *opaque, EEL_F *dest_device);
    static EEL_F NSEEL_CGEN_CALL _osc_parm(void *opaque, EEL_F *parmidx, EEL_F *typeptr);
    static EEL_F NSEEL_CGEN_CALL _osc_match(void *opaque, EEL_F *fmt);
    static EEL_F NSEEL_CGEN_CALL _printf(void *opaque, EEL_F *fmt);
};


WDL_PtrList<scriptInstance> g_scripts;
WDL_PtrList<inputDevice> g_inputs; // these are owned here, scriptInstances reference them
WDL_PtrList<outputDevice> g_outputs;

class oscInputDevice : public inputDevice
{
public:
  oscInputDevice(struct sockaddr_in addr)
  {
    m_recvaddr = addr;
    m_recvsock=socket(AF_INET, SOCK_DGRAM, 0);
    if (m_recvsock>=0)
    {
      int on=1;
      setsockopt(m_recvsock, SOL_SOCKET, SO_BROADCAST, (char*)&on, sizeof(on));
      if (!bind(m_recvsock, (struct sockaddr*)&m_recvaddr, sizeof(struct sockaddr))) 
      {
        SET_SOCK_BLOCK(m_recvsock, false);
      }
      else
      {
        closesocket(m_recvsock);
        m_recvsock=-1;
      }
    }
  }
  virtual ~oscInputDevice()
  {
    if (m_recvsock >= 0)
    {
      shutdown(m_recvsock, SHUT_RDWR);
      closesocket(m_recvsock);
      m_recvsock=-1;
    }
  }

  virtual void start() {  }

  virtual void run(WDL_FastString &textOut)
  {
    for (;;)
    {
      char buf[16384];
      buf[0]=0;
      const int len=(int)recvfrom(m_recvsock, buf, sizeof(buf), 0, 0, 0);
      if (len<1) break;

      onMessage(1,(const unsigned char *)buf,len);
    }
  }

  virtual const char *get_type() { return "OSC"; }

  int m_recvsock;
  struct sockaddr_in m_recvaddr;
  WDL_Queue m_recvq;

};

class oscOutputDevice : public outputDevice
{
public:
  oscOutputDevice(const char *dest, int maxpacket, int sendsleep) 
  {
    memset(&m_sendaddr, 0, sizeof(m_sendaddr));

    m_dest.Set(dest);

    WDL_String tmp(dest);
    int sendport=0;
    char *p=strstr(tmp.Get(),":");
    if (p) 
    {
      *p++=0;
      sendport=atoi(p);
    }
    if (!sendport) sendport=8000;

    m_sendaddr.sin_family=AF_INET;
    m_sendaddr.sin_addr.s_addr=inet_addr(tmp.Get());
    m_sendaddr.sin_port=htons(sendport);

    m_maxpacketsz = maxpacket> 0 ? maxpacket:1024;
    m_sendsleep = sendsleep >= 0 ? sendsleep : 10;
    m_sendsock=socket(AF_INET, SOCK_DGRAM, 0);
    if (m_sendsock >= 0)
    {
      int on=1;
      setsockopt(m_sendsock, SOL_SOCKET, SO_BROADCAST, (char*)&on, sizeof(on));
    }

  }
  virtual ~oscOutputDevice() 
  { 
    if (m_sendsock >= 0)
    {
      shutdown(m_sendsock, SHUT_RDWR);
      closesocket(m_sendsock);
      m_sendsock=-1;
    }
  } 

  virtual void run(WDL_FastString &results)
  {
    static char hdr[16] = { '#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0, 0, 0, 1, 0, 0, 0 };

    // send m_sendq as UDP blocks
    if (m_sendq.Available()<1) return;

    if (m_sendq.Available()<=16)
    {
      m_sendq.Clear();
      return;
    }
    // m_sendq should begin with a 16 byte pad, then messages in OSC

    char* packetstart=(char*)m_sendq.Get();
    int packetlen=16;
    bool hasbundle=false;
    m_sendq.Advance(16); // skip bundle for now, but keep it around

    while (m_sendq.Available() >= sizeof(int))
    {
      int len=*(int*)m_sendq.Get(); // not advancing
      OSC_MAKEINTMEM4BE((char*)&len);

      if (len < 1 || len > MAX_OSC_MSG_LEN || len > m_sendq.Available()) break;             
        
      if (packetlen > 16 && packetlen+sizeof(int)+len > m_maxpacketsz)
      {
        // packet is full
        if (!hasbundle)
        {
          packetstart += 20;
          packetlen -= 20;
        }
        else
        {
          memcpy(packetstart,hdr,16);
        }

        sendto(m_sendsock, packetstart, packetlen, 0, (struct sockaddr*)&m_sendaddr, sizeof(struct sockaddr));
        if (m_sendsleep>0) Sleep(m_sendsleep);

        packetstart=(char*)m_sendq.Get()-16; // safe since we padded the queue start
        packetlen=16;
        hasbundle=false;
      }
     
      if (packetlen > 16) hasbundle=true;
      m_sendq.Advance(sizeof(int)+len);
      packetlen += sizeof(int)+len;
    }

    if (packetlen > 16)
    {
      if (!hasbundle)
      {
        packetstart += 20;
        packetlen -= 20;
      }
      else
      {
        memcpy(packetstart,hdr,16);
      }
      sendto(m_sendsock, packetstart, packetlen, 0, (struct sockaddr*)&m_sendaddr, sizeof(struct sockaddr));
      if (m_sendsleep>0) Sleep(m_sendsleep);
    }

    m_sendq.Clear();
  }

  virtual void oscSend(const char *src, int len)
  {
    if (!m_sendq.GetSize()) m_sendq.Add(NULL,16);

    int tlen=len;
    OSC_MAKEINTMEM4BE(&tlen);
    m_sendq.Add(&tlen,sizeof(tlen));
    m_sendq.Add(src,len);
  }
  virtual const char *get_type() { return "OSC"; }

  int m_sendsock;
  int m_maxpacketsz, m_sendsleep;
  struct sockaddr_in m_sendaddr;
  WDL_Queue m_sendq;
  WDL_String m_dest;
};



void scriptInstance::compileCode(int parsestate, const WDL_FastString &curblock, WDL_FastString &results, int lineoffs)
{
  if (parsestate<0 || parsestate >= sizeof(m_code)/sizeof(m_code[0])) return;

  if (m_code[parsestate])
  {
    results.AppendFormatted(1024,"\tWarning: duplicate %s sections, ignoring later copy\r\n",g_code_names[parsestate]);
    return;
  }


  WDL_FastString procOut;
  WDL_FastString newstr;
  const char *rdptr = curblock.Get();
  // preprocess to get strings from "", and replace with an index of STRING_INDEX_BASE+m_strings.GetSize()
  int state=0; 
  // states:
  // 1 = comment to end of line
  // 2=comment til */
  while (*rdptr)
  {
    switch (state)
    {
      case 0:
        if (*rdptr == '/')
        {
          if (rdptr[1] == '/') state=1;
          else if (rdptr[1] == '*') state=2;
        }

        if (*rdptr == '"')
        {
          // scan tokens and replace with (idx)
          newstr.Set("");

          rdptr++; 

          int esc_state=0;
          while (*rdptr)
          {
            if (esc_state==0)
            {
              if (*rdptr == '\\') 
              {
                esc_state=1;
              }
              else if (*rdptr == '"')
              {
                if (rdptr[1] != '"') break;
                // "" converts to "
                rdptr++;
              }
            }
            else 
            {
              if (*rdptr != '"') newstr.Append("\\",1);
              esc_state=0;
            }

            if (!esc_state) newstr.Append(rdptr,1);
            rdptr++;
          }

          if (*rdptr) rdptr++; // skip trailing quote

          procOut.AppendFormatted(128,"(%d)",STRING_INDEX_BASE+m_strings.GetSize());
          m_strings.Add(strdup(newstr.Get()));

          continue;
        }
      break;
      case 1:
        if (*rdptr == '\n') state=0;
      break;
      case 2:
        if (*rdptr == '*' && rdptr[1] == '/') 
        {
          procOut.Append(rdptr++,1);
          state=0;
        }
      break;
    }
    if (state<3) procOut.Append(rdptr,1);

    rdptr++;
  }

  m_code[parsestate]=NSEEL_code_compile_ex(m_vm,procOut.Get(),lineoffs,NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS);  

  char *err;
  if (!m_code[parsestate] && (err=NSEEL_code_getcodeerror(m_vm)))
  {
    results.AppendFormatted(1024,"\tError: in %s: %s\r\n",g_code_names[parsestate], err);
  }
}

static int validate_format_specifier(const char *fmt_in, char *typeOut)
{
  const char *fmt = fmt_in+1;
  int state=0;
  if (fmt_in[0] != '%') return 0; // ugh passed a non-specifier
  while (*fmt)
  {
    const char c = *fmt++;
    if (c == 'f'|| c=='e' || c=='E' || c=='g' || c=='G' || c == 'd' || c == 'u' || c == 'x' || c == 'X' || c == 'c' || c =='s' || c=='S') 
    {     
      if (typeOut) *typeOut = c;
      return fmt - fmt_in;
    }
    else if (c == '.') 
    {
      if (state&2) break;
      state |= 2;
    }
    else if (c == '+') 
    {
      if (state&(32|16|8|4)) break;
      state |= 8;
    }
    else if (c == '-') 
    {
      if (state&(32|16|8|4)) break;
      state |= 16;
    }
    else if (c == ' ') 
    {
      if (state&(32|16|8|4)) break;
      state |= 32;
    }
    else if (c >= '0' && c <= '9') 
    {
      state|=4;
    }
    else 
    {
      break; // %unknown-char
    }
  }
  return 0;

}



bool scriptInstance::format_strings(const char *fmt, char *buf, int buf_sz, int want_escapes, const WDL_PtrList<char> *stringTab)
{
  bool rv=true;
  int fmt_parmpos = 0;
  char *op = buf;
  while (*fmt && op < buf+buf_sz-128)
  {
    if (fmt[0] == '%' && fmt[1] == '%') 
    {
      *op++ = '%';
      fmt+=2;
    }
    else if (fmt[0] == '%')
    {
      char ct=0;
      const int l=validate_format_specifier(fmt,&ct);
      char fs[128];
      if (!l || !ct || l >= sizeof(fs)) 
      {
        rv=false;
        break;
      }
      lstrcpyn(fs,fmt,l+1);

      const double v = fmt_parmpos < MAX_OSC_FMTS && m_var_fmt[fmt_parmpos] ? m_var_fmt[fmt_parmpos][0] : 0.0;
      fmt_parmpos++;

      if (ct == 's' || ct=='S')
      {
        const char *str = stringTab ? stringTab->Get((int) (v+0.5) - STRING_INDEX_BASE) : NULL;
        lstrcpyn_safe(op,str ? str : "",100);
      }
      else if (ct == 'x' || ct == 'X' || ct == 'd' || ct == 'u')
      {
        snprintf(op,64,fs,(int) (v));
      }
      else if (ct == 'c')
      {
        char c = (char) (int)(v);
        if (!c) c=' ';
        snprintf(op,64,fs,c);
      }
      else
        snprintf(op,64,fs,v);

      while (*op) op++;

      fmt += l;
    }
    else 
    {
      if (want_escapes && fmt[0] == '\\')
      {
        if (fmt[1] == '\\') { *op++ = '\\'; fmt+=2; }
        else if (fmt[1] == 'n' || fmt[1] == 'N')  
        { 
          if (want_escapes & 2) *op++ = '\r';
          *op++ = '\n'; fmt+=2; 
        }
        else if (fmt[1] == 'r' || fmt[1] == 'R')  { if (!(want_escapes & 2)) *op++ = '\r'; fmt+=2; }
        else if (fmt[1] == 't' || fmt[1] == 't')  { *op++ = '\t'; fmt+=2; }
        else if (fmt[1] == 'x' || fmt[1]=='X' || (fmt[1] >= '0' && fmt[1] <= '9'))
        {
          int base = 10;
          fmt++;
          if (fmt[0] == 'x' || fmt[0] == 'X') { fmt++; base=16; }
          else if (fmt[0] == '0') base=8;

          int c=0;
          char thisc=toupper(*fmt);
          while ((thisc >= '0' && thisc <= (base>=10 ? '9' : '7')) ||
                 (base == 16 && thisc >= 'A' && thisc <= 'F')
                 )
          {
            c *= base;
            if (thisc >= 'A' && thisc <= 'F')
              c+=thisc - 'A' + 10;
            else
              c += thisc - '0';

            fmt++;
            thisc=toupper(*fmt);
          }
          *op++ = (char)c;
        }
        else *op++ = *fmt++;
      }
      else
      {
        *op++ = *fmt++;
      }
    }

  }
  *op=0;
  return rv;
}

EEL_F NSEEL_CGEN_CALL scriptInstance::_printf(void *opaque, EEL_F *fmt_index)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (_this && _this->m_debugOut)
  {
    const char *fmt = _this->m_strings.Get((int) (*fmt_index + 0.5) - STRING_INDEX_BASE );
    if (fmt)
    {
      char buf[1024+128];
      if (_this->format_strings(fmt,buf,sizeof(buf), 2, &_this->m_strings))
      {
        _this->m_debugOut->Append(buf);
        return 1.0;
      }
      else
      {
        _this->m_debugOut->AppendFormatted(512,"_printf: bad format string %s\n",fmt);
      }
    }
    else
    {
      _this->m_debugOut->AppendFormatted(512,"_printf: bad format specifier passed %f\n",*fmt_index);
    }
  }
  return 0.0;
}

EEL_F NSEEL_CGEN_CALL scriptInstance::_send_oscevent(void *opaque, EEL_F *dest_device, EEL_F *fmt_index, EEL_F *value)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (_this)
  {
    int output_idx = (int) floor(*dest_device+0.5);
    outputDevice *output = _this->m_out_devs.Get(output_idx - OUTPUT_INDEX_BASE);
    if (!output && output_idx >= 0)
    {
      if (_this->m_debugOut) _this->m_debugOut->AppendFormatted(512,"oscsend(): output device %f invalid\r\n",*dest_device);
    }
    if (output || output_idx == -1 || output_idx==-100)
    {
      const char *fmt = _this->m_strings.Get((int) (*fmt_index + 0.5) - STRING_INDEX_BASE );
      if (fmt)
      {
        char fmt_type = 0;
        if (fmt[0] && fmt[0] != '/') fmt_type = *fmt++;

        char buf[1024+128];
        if (_this->format_strings(fmt,buf,sizeof(buf), 0, &_this->m_strings))
        {
          OscMessageWrite wr;
          wr.PushWord(buf);
        
          if (fmt_type == 'b') wr.PushIntArg(!!(int) (*value));
          else if (fmt_type =='i') wr.PushIntArg((int) (*value));
          else if (fmt_type == 's')
          {
            const char *strval = _this->m_strings.Get((int) (*value + 0.5) - STRING_INDEX_BASE );
            if (strval)
            {
              wr.PushStringArg(strval);
            }
            else
            {
              char tmp[64];
              sprintf(tmp,"%.2f",*value);
              wr.PushStringArg(tmp);
            }         
          }
          else if (fmt_type == 't')
          {
            // no parameter, just toggle
          }
          else 
          {
            // default to float
            wr.PushFloatArg((float)*value);
          }

          int l=0;
          const char *ret=wr.GetBuffer(&l);
          if (ret && l>0) 
          {
            if (output)
            {
              output->oscSend(ret,l);
            }
            else if (output_idx==-100)
            {
              int n;
              for (n=0;n<g_outputs.GetSize();n++)
                g_outputs.Get(n)->oscSend(ret,l);
            }
            else 
            {
              int n;
              for (n=0;n<_this->m_out_devs.GetSize();n++)
                _this->m_out_devs.Get(n)->oscSend(ret,l);
            }
          }
          return 1.0;
        }
        else
        {
          if (_this->m_debugOut) _this->m_debugOut->AppendFormatted(512,"oscsend(): bad format string '%s'\r\n",fmt);
        }
      }
      else
      {
        if (_this->m_debugOut) _this->m_debugOut->AppendFormatted(512,"oscsend(): bad format index %f\r\n",*fmt_index);
      }
    }
  }
  return 0.0;
}


EEL_F NSEEL_CGEN_CALL scriptInstance::_osc_parm(void *opaque, EEL_F *parmidx, EEL_F *typeptr)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  *typeptr = 0.0;
  if (_this && _this->m_cur_oscmsg)
  {
    const int idx = (int) (*parmidx);

    char c=0;
    const void *ptr=_this->m_cur_oscmsg->GetIndexedArg(idx&65535,&c);
    if (!ptr) return 0.0;

    *typeptr = (EEL_F)c;
    if (c=='f') return (EEL_F) *(const float *)ptr;
    if (c=='i') return (EEL_F) *(const int *)ptr;
    if (c=='s') 
    {
      const char *s=(const char *)ptr;
      int idx2=(idx>>16)&1023;
      while (idx2>0 && *s) { s++; idx2--; }
      return (EEL_F)*s;
    }
  }
  return 0.0;
}
EEL_F NSEEL_CGEN_CALL scriptInstance::_osc_match(void *opaque, EEL_F *fmt_index)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (_this && _this->m_cur_oscmsg)
  {
    const char *fmt = _this->m_strings.Get((int) (*fmt_index + 0.5) - STRING_INDEX_BASE );
    if (fmt)
    {
      const char *msg = _this->m_cur_oscmsg->GetMessage();
      if (msg)
      {
        int match_fmt_pos=0;
        // check for match, updating m_var_fmt[*] as necessary
        // %d=12345
        // %f=12345[.678]
        // %c=any nonzero char, ascii value
        // %x=12354ab
        // %*, %?, %+, %% literals
        // * ? +  match minimal groups of 0+,1, or 1+ chars
        for (;;)
        {
          const char fmtc = *fmt;
          const char msgc = *msg;
          if (!fmtc || !msgc) return fmtc==msgc ? 1.0 : 0.0;

          switch (fmtc)
          {
            case '*':
            case '+':
              {
                const char *nc = ++fmt;
                if (!*nc) return 1.0; // match!

                int nc_len=0;
                while (nc[nc_len] && 
                       nc[nc_len] != '%' && 
                       nc[nc_len] != '*' &&
                       nc[nc_len] != '+' &&
                       nc[nc_len] != '?') nc_len++;
                if (!nc_len) continue;

                if (fmtc == '+') msg++; // require at least a single char to skip for +

                while (*msg && strnicmp(msg,nc,nc_len)) msg++;
              }
            break;
            case '?':
              fmt++;
              msg++;
            break;
            case '%':
              {
                const char fmt_char = fmt[1];
                fmt+=2;

                if (!fmt_char) return 0.0; // malformed

                if (fmt_char == '*' || 
                    fmt_char == '?' || 
                    fmt_char == '+' || 
                    fmt_char == '%')
                {
                  if (msgc != fmt_char) return 0.0;
                  msg++;
                }
                else if (fmt_char == 'c')
                {
                  if (!msg[0]) return 0.0;
                  if (match_fmt_pos < MAX_OSC_FMTS && _this->m_var_fmt[match_fmt_pos])
                    _this->m_var_fmt[match_fmt_pos][0] = (EEL_F)msg[0];
                  msg++;
                }
                else if (fmt_char == 'd' || fmt_char == 'u')
                {
                  int len=0;
                  while (msg[len] >= '0' && msg[len] <= '9') len++;
                  if (!len) return 0.0;

                  if (match_fmt_pos < MAX_OSC_FMTS && _this->m_var_fmt[match_fmt_pos])
                  {
                    char *bl=(char*)msg;
                    if (fmt_char == 'd') 
                      _this->m_var_fmt[match_fmt_pos][0] = (EEL_F)atoi(msg);
                    else
                      _this->m_var_fmt[match_fmt_pos][0] = (EEL_F)strtoul(msg,&bl,10);
                  }

                  msg+=len;
                }
                else if (fmt_char == 'x' || fmt_char == 'X')
                {
                  int len=0;
                  while ((msg[len] >= '0' && msg[len] <= '9') ||
                         (msg[len] >= 'A' && msg[len] <= 'F') ||
                         (msg[len] >= 'a' && msg[len] <= 'f')
                         ) len++;
                  if (!len) return 0.0;

                  if (match_fmt_pos < MAX_OSC_FMTS && _this->m_var_fmt[match_fmt_pos])
                  {
                    char *bl=(char*)msg;
                    _this->m_var_fmt[match_fmt_pos][0] = (EEL_F)strtoul(msg,&bl,16);
                  }

                  msg+=len;
                }
                else if (fmt_char == 'f')
                {
                  int len=0;
                  while (msg[len] >= '0' && msg[len] <= '9') len++;
                  if (msg[len] == '.') 
                  { 
                    len++; 
                    while (msg[len] >= '0' && msg[len] <= '9') len++;
                  }
                  if (!len) return 0.0;

                  if (match_fmt_pos < MAX_OSC_FMTS && _this->m_var_fmt[match_fmt_pos])
                    _this->m_var_fmt[match_fmt_pos][0] = (EEL_F)atof(msg);

                  msg+=len;
                }
              }
            break;
            default:
              if (toupper(fmtc) != toupper(msgc)) return 0.0;
              fmt++;
              msg++;
            break;
          }

        }
      }
    }
  }
  return 0.0;
}


EEL_F NSEEL_CGEN_CALL scriptInstance::_send_midievent(void *opaque, EEL_F *dest_device)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (_this)
  {
    int output_idx = (int) floor(*dest_device+0.5);
    outputDevice *output = _this->m_out_devs.Get(output_idx - OUTPUT_INDEX_BASE);
    if (!output && output_idx>=0)
    {
      if (_this->m_debugOut) _this->m_debugOut->AppendFormatted(512,"midisend(): device %f invalid\r\n",*dest_device);
    }
    if (output || output_idx == -1 || output_idx==-100)
    {
      unsigned char msg[3];
      EEL_F **vms = _this->m_var_msgs;
      msg[0] = vms[0] ? (int) (vms[0][0]) : 0;
      msg[1] = vms[1] ? (int) (vms[1][0]) : 0;
      msg[2] = vms[2] ? (int) (vms[2][0]) : 0;

      if (output)
      {
        output->midiSend(msg,3);
      }
      else if (output_idx==-100)
      {
        int n;
        for (n=0;n<g_outputs.GetSize();n++)
          g_outputs.Get(n)->midiSend(msg,3);
      }
      else 
      {
        int n;
        for (n=0;n<_this->m_out_devs.GetSize();n++)
          _this->m_out_devs.Get(n)->midiSend(msg,3);
      }
      return 1.0;
    }
  }
  return 0.0;
}


void scriptInstance::load_script(WDL_FastString &results)
{
  clear();

  m_vm = NSEEL_VM_alloc();
  NSEEL_VM_SetCustomFuncThis(m_vm,this);

  m_var_time = NSEEL_VM_regvar(m_vm,"time");
  m_var_msgs[0] = NSEEL_VM_regvar(m_vm,"msg1");
  m_var_msgs[1] = NSEEL_VM_regvar(m_vm,"msg2");
  m_var_msgs[2] = NSEEL_VM_regvar(m_vm,"msg3");
  m_var_msgs[3] = NSEEL_VM_regvar(m_vm,"msgdev");

  int x;
  for (x=0;x<MAX_OSC_FMTS;x++)
  {
    char tmp[32];
    sprintf(tmp,"fmt%d",x);
    m_var_fmt[x] = NSEEL_VM_regvar(m_vm,tmp);
  }

  results.Append(m_fn.Get());
  results.Append("\r\n");

  FILE *fp = fopen(m_fn.Get(),"r");
  if (!fp)
  {
    results.Append("\tError: failed opening filed.");
    return;
  }

  bool comment_state=false;
  int parsestate=-1,cursec_lineoffs=0,lineoffs=0;
  WDL_FastString curblock;
  for (;;)
  {
    char linebuf[8192];
    linebuf[0]=0;
    fgets(linebuf,sizeof(linebuf),fp);
    lineoffs++;

    if (!linebuf[0]) break;

    {
      char *p=linebuf;
      while (*p) p++;
      p--;
      while (p >= linebuf && (*p == '\r' || *p == '\n')) { *p=0; p--; }
    }

    LineParser lp(comment_state);
    if (linebuf[0] && !lp.parse(linebuf) && lp.getnumtokens()> 0 && lp.gettoken_str(0)[0] == '@')
    {
      const char *tok=lp.gettoken_str(0);
      int x;
      for (x=0;x<sizeof(g_code_names)/sizeof(g_code_names[0]) && strcmp(tok,g_code_names[x]);x++);

      if (x < sizeof(g_code_names)/sizeof(g_code_names[0]))
      {
        compileCode(parsestate,curblock,results,cursec_lineoffs);
        parsestate=x;
        cursec_lineoffs=lineoffs;
        curblock.Set("");
      }
      else if (!strcmp(tok,"@input"))
      {
        if (lp.getnumtokens()<3 || !lp.gettoken_str(1)[0])
        {
          results.Append("\tUsage: @input devicehandle \"substring devicename match\" [skip_count]\r\n");
          results.Append("\tUsage: @input devicehandle \"MIDI:substring devicename match\" [skip_count]\r\n");
          results.Append("\tUsage: @input devicehandle \"OSC:1.2.3.4:port\"\r\n");
          results.Append("\tUsage: @input devicehandle \"OSC:*:port\"\r\n");
        }
        else
        {
          if (NSEEL_VM_get_var_refcnt(m_vm,lp.gettoken_str(1))>=0)
          {
            results.AppendFormatted(1024,"\tWarning: device name '%s' already in use, skipping @input line\r\n",lp.gettoken_str(1));
          }
          else
          {
            const char *dp = lp.gettoken_str(2);
            if (!strnicmp(dp,"OSC:",4))
            {
              dp += 4;
              while (*dp == ' ') dp++;

              char buf[512];
              lstrcpyn_safe(buf,dp,sizeof(buf));
              char *portstr = strstr(buf,":");
              int port = 0;
              if (portstr)
              {
                *portstr++ = 0;
                port = atoi(portstr);
              }

              struct sockaddr_in addr;
              addr.sin_addr.s_addr=INADDR_ANY;
              addr.sin_family=AF_INET;
              if (buf[0] && buf[0] != '*') addr.sin_addr.s_addr = inet_addr(buf);
              if (addr.sin_addr.s_addr == INADDR_NONE) addr.sin_addr.s_addr = INADDR_ANY;
              addr.sin_port=htons(port);

              int x;
              bool is_reuse=false;
              oscInputDevice *r=NULL;
              for (x=0; x < g_inputs.GetSize(); x++)
              {
                inputDevice *dev = g_inputs.Get(x);
                if (dev && !strcmp(dev->get_type(),"OSC"))
                {
                  oscInputDevice *od = (oscInputDevice *)dev;
                  if (od->m_recvaddr.sin_port == addr.sin_port && od->m_recvaddr.sin_addr.s_addr == addr.sin_addr.s_addr)
                  {
                    r=od;
                    is_reuse=true;

                    results.AppendFormatted(1024,"\tAttached to already-opened listener '%s'\r\n",dp);

                    break;
                  }
                }
              }
              if (!r)
              {
                r = new oscInputDevice(addr);
                if (r->m_recvsock < 0)
                {
                  delete r;
                  r=NULL;
                  results.AppendFormatted(1024,"\tError listening for '%s'\r\n",dp);
                }
                else
                {
                  results.AppendFormatted(1024,"\tListening on '%s'\r\n",dp);
                }
              }
              if (r)
              {
                EEL_F *dev_idx = NSEEL_VM_regvar(m_vm,lp.gettoken_str(1));
                if (dev_idx) dev_idx[0] = m_in_devs.GetSize() + INPUT_INDEX_BASE;
                r->addinst(messageCallback,this,dev_idx);
                m_in_devs.Add(r);

                if (!is_reuse) g_inputs.Add(r);
              }


            }
            else
            {
              if (!strnicmp(dp,"MIDI:",5))
              {
                dp += 5;
                while (*dp == ' ') dp++;
              }
              const char *substr = dp;
              int skipcnt = lp.getnumtokens()>=3 ? lp.gettoken_int(3) : 0;

              midiInputDevice *rec = new midiInputDevice(substr,skipcnt, &g_inputs);
              bool is_reuse=false;
              if (!rec->m_handle)
              {
                if (rec->m_open_would_use_altdev)
                {
                  midiInputDevice *tmp=rec->m_open_would_use_altdev;
                  delete rec;
                  rec=tmp;
                  is_reuse=true;

                  if (!rec->m_handle)
                    results.AppendFormatted(1024,"\tWarning: attached to un-opened device '%s'\r\n",rec->m_name_used);
                  else
                    results.AppendFormatted(1024,"\tAttached to already-opened device '%s'\r\n",rec->m_name_used);
                }
                else if (rec->m_name_used)
                  results.AppendFormatted(1024,"\tWarning: tried to open device '%s' but failed\r\n",rec->m_name_used);
                else
                  results.AppendFormatted(1024,"\tWarning: tried to open device matching '%s'(%d) but failed, will retry\r\n",substr,skipcnt);
              }
              EEL_F *dev_idx = NSEEL_VM_regvar(m_vm,lp.gettoken_str(1));
              if (dev_idx) dev_idx[0] = m_in_devs.GetSize() + INPUT_INDEX_BASE;
              rec->addinst(messageCallback,this,dev_idx);
              m_in_devs.Add(rec);

              if (!is_reuse) g_inputs.Add(rec);
            }
          }
        }
      }
      else if (!strcmp(tok,"@output"))
      {
        if (lp.getnumtokens()<3 || !lp.gettoken_str(1)[0])
        {
          results.Append("\tUsage: @output devicehandle \"host:port\" [maxpacketsize (def=1024)] [sleepinMS (def=10)]\r\n");
          results.Append("\tUsage: @output devicehandle \"OSC:host:port\" [maxpacketsize (def=1024)] [sleepinMS (def=10)]\r\n");
          results.Append("\tUsage: @output devicehandle \"MIDI:substring match\" [skip]\r\n");
        }
        else
        {
          if (NSEEL_VM_get_var_refcnt(m_vm,lp.gettoken_str(1))>=0)
          {
            results.AppendFormatted(1024,"\tWarning: device name '%s' already in use, skipping @output line\r\n",lp.gettoken_str(1));
          }
          else
          {
            const char *dp=lp.gettoken_str(2);
            if (!strnicmp(dp,"MIDI:",5))
            {
              dp += 5;
              while (*dp == ' ') dp++;
              const char *substr = dp;
              int skipcnt = lp.getnumtokens()>=3 ? lp.gettoken_int(3) : 0;

              bool is_reuse=false;
              midiOutputDevice *rec = new midiOutputDevice(substr,skipcnt, &g_outputs);
              if (!rec->m_handle)
              {
                if (rec->m_open_would_use_altdev)
                {
                  midiOutputDevice *tmp=rec->m_open_would_use_altdev;
                  delete rec;
                  rec=tmp;
                  is_reuse=true;

                  if (!rec->m_handle)
                    results.AppendFormatted(1024,"\tWarning: attached to un-opened device '%s'\r\n",rec->m_name_used);
                  else
                    results.AppendFormatted(1024,"\tAttached to already-opened device '%s'\r\n",rec->m_name_used);
                }
                else if (rec->m_name_used)
                  results.AppendFormatted(1024,"\tWarning: tried to open device '%s' but failed\r\n",rec->m_name_used);
                else
                  results.AppendFormatted(1024,"\tWarning: tried to open device matching '%s'(%d) but failed, will retry\r\n",substr,skipcnt);
              }

              EEL_F *var=NSEEL_VM_regvar(m_vm,lp.gettoken_str(1));
              if (var) *var=m_out_devs.GetSize() + OUTPUT_INDEX_BASE;
              m_out_devs.Add(rec);

              if (!is_reuse) g_outputs.Add(rec);


            }
            else
            {
              if (!strnicmp(dp,"OSC:",4)) 
              {
                dp+=4;
                while (*dp == ' ') dp++;
              }
              oscOutputDevice *r = NULL;
              int x;
              bool is_reuse=false;
              for (x=0;x<g_outputs.GetSize();x++)
              {
                outputDevice *d = g_outputs.Get(x);
                if (d && !strcmp(d->get_type(),"OSC"))
                {
                  oscOutputDevice *p = (oscOutputDevice *)d;
                  if (!strcmp(p->m_dest.Get(),dp))
                  {
                    is_reuse=true;
                    r=p; // reuse!
                    break;
                  }
                }
              }

              if (!r) 
              {
                is_reuse=false;
                r = new oscOutputDevice(dp, lp.getnumtokens()>=4 ? lp.gettoken_int(3) : 0,
                                            lp.getnumtokens()>=5 ? lp.gettoken_int(4) : -1);
                if (r->m_sendsock<0)
                {
                  results.AppendFormatted(1024,"\tWarning: failed creating destination for @output '%s' -> '%s'\r\n",lp.gettoken_str(1),lp.gettoken_str(2));
                  delete r;
                  r=NULL;
                }
              }

              if (r)
              {
                EEL_F *var=NSEEL_VM_regvar(m_vm,lp.gettoken_str(1));
                if (var) *var=m_out_devs.GetSize() + OUTPUT_INDEX_BASE;
                m_out_devs.Add(r);

                if (!is_reuse) g_outputs.Add(r);
              }
            }
          }
        }
      }
      else
      {
        results.AppendFormatted(1024,"\tWarning: Unknown directive: %s\r\n",tok);
      }
    }
    else
    {
      const char *p=linebuf;
      if (parsestate==-3)
      {
        while (*p)
        {
          if (p[0] == '*' && p[1]== '/')
          {
            parsestate=-1; // end of comment!
            p+=2;
            break;
          }
        }
      }
      if (parsestate==-1 && p[0])
      {
        while (*p == ' ' || *p =='\t') p++;
        if (!*p || (p[0] == '/' && p[1] == '/'))
        {
        }
        else
        {
          if (*p == '/' && p[1] == '*')
          {
            parsestate=-3;
          }
          else
          {
            results.AppendFormatted(1024,"\tWarning: line '%.100s' (and possibly more)' are not in valid section and may be ignored\r\n", linebuf);
            parsestate=-2;
          }
        }
      }
      if (parsestate>=0)
      {
        curblock.Append(linebuf);
        curblock.Append("\n");
      }
    }
  }
  compileCode(parsestate,curblock,results,cursec_lineoffs);


  fclose(fp);

  if (!m_in_devs.GetSize()) results.Append("\tWarning: No @input specified\r\n");
  if (!m_out_devs.GetSize()) results.Append("\tWarning: No @output specified\r\n");

  results.AppendFormatted(512,"\t%d inputs, %d outputs, %d strings\r\n\r\n",
      m_in_devs.GetSize(),m_out_devs.GetSize(),m_strings.GetSize());

}

void scriptInstance::messageCallback(void *d1, void *d2, char type, int len, void *msg)
{
  scriptInstance *_this  = (scriptInstance *)d1;
  if (_this && msg)
  {
    // MIDI
    if (_this->m_incoming_events.GetSize() < 65536)
    {
      const int this_sz = ((sizeof(incomingEvent) + (len-3)) + 7) & ~7;

      _this->m_incoming_events_mutex.Enter();
      const int oldsz = _this->m_incoming_events.GetSize();
      _this->m_incoming_events.Resize(oldsz + this_sz,false);
      if (_this->m_incoming_events.GetSize() == oldsz+this_sz)
      {
        incomingEvent *item=(incomingEvent *) ((char *)_this->m_incoming_events.Get() + oldsz);
        item->dev_ptr = (EEL_F*)d2;
        item->sz = len;
        item->type = type;
        memcpy(item->msg,msg,len);
      }
      _this->m_incoming_events_mutex.Leave();
    }
  }
}

bool scriptInstance::run(double curtime, WDL_FastString &results)
{
  bool rv=false;
  if (m_var_time) *m_var_time = curtime;


  m_debugOut = &results;
  if (m_incoming_events.GetSize())
  {
    static WDL_HeapBuf tmp;

    m_incoming_events_mutex.Enter();
    tmp.CopyFrom(&m_incoming_events,false);
    m_incoming_events.Resize(0,false);
    m_incoming_events_mutex.Leave();

    int pos=0;
    const int endpos = tmp.GetSize();
    while (pos < endpos+1 - sizeof(incomingEvent))
    {
      incomingEvent *evt = (incomingEvent*) ((char *)tmp.Get()+pos);
      
      const int this_sz = ((sizeof(incomingEvent) + (evt->sz-3)) + 7) & ~7;

      if (pos+this_sz > endpos) break;
      pos += this_sz;

      switch (evt->type)
      {
        case 0:
          if (evt->sz == 3)
          {
            int asInt = (evt->msg[0] << 16) | (evt->msg[1] << 8) | evt->msg[2];
            if (g_recent_events[0] != asInt)
            {
              memmove(g_recent_events+1,g_recent_events,sizeof(g_recent_events)-sizeof(g_recent_events[0]));
              g_recent_events[0]=asInt;
              rv=true;
            }

            if (m_var_msgs[0]) m_var_msgs[0][0] = evt->msg[0];
            if (m_var_msgs[1]) m_var_msgs[1][0] = evt->msg[1];
            if (m_var_msgs[2]) m_var_msgs[2][0] = evt->msg[2];
            if (m_var_msgs[3]) m_var_msgs[3][0] = evt->dev_ptr ? *evt->dev_ptr : -1.0;
            NSEEL_code_execute(m_code[2]);
          }
        break;
        case 1:
          if (m_code[3])
          {
            int rd_pos = 0;
            int rd_sz = evt->sz;
            if (evt->sz > 20 && !strcmp((char*)evt->msg, "#bundle"))
            {
              rd_sz = *(int *)(evt->msg+16);
              OSC_MAKEINTMEM4BE(&rd_sz);
              rd_pos += 20;
            }
            while (rd_pos + rd_sz <= evt->sz && rd_sz>=0)
            {
              OscMessageRead rmsg((char*)evt->msg + rd_pos, rd_sz);

              const char *mstr = rmsg.GetMessage();
              if (mstr && *mstr)
              {
                m_cur_oscmsg = &rmsg;
                NSEEL_code_execute(m_code[3]);
                m_cur_oscmsg = NULL;
              }

              rd_pos += rd_sz+4;
              if (rd_pos >= evt->sz) break;

              rd_sz = *(int *)(evt->msg+rd_pos-4);
              OSC_MAKEINTMEM4BE(&rd_sz);
            }

          }
        break;
      }
    }
  }
  if (m_vm && m_code[1]) NSEEL_code_execute(m_code[1]); // timer follows messages
  m_debugOut=0;
  return rv;
}



void NSEEL_HOSTSTUB_EnterMutex() { }
void NSEEL_HOSTSTUB_LeaveMutex() { }

void UpdateLogText(const WDL_FastString &str, HWND dest, bool doScroll)
{
  if (doScroll)
  {
  #ifdef _WIN32
    SendMessage(dest,WM_SETREDRAW,0,0);
    SetWindowText(dest,str.Get());
    SCROLLINFO si={sizeof(si),SIF_RANGE|SIF_POS|SIF_TRACKPOS,};
    GetScrollInfo(dest,SB_VERT,&si);
    SendMessage(dest, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION,si.nMax),0);
    SendMessage(dest,WM_SETREDRAW,1,0);
    InvalidateRect(dest,NULL,FALSE);

  #else
    SetWindowText(dest,str.Get());
    SendMessage(dest,EM_SCROLL,SB_BOTTOM,0);
  #endif
  }
  else
  {
    SetWindowText(dest,str.Get());
  }
}

void load_scripts_for_path(const char *path, WDL_FastString &results)
{
  WDL_DirScan ds;
  WDL_String s;
  if (!ds.First(path))
  {
    do
    {
      const char *fn = ds.GetCurrentFN();
      if (fn[0] != '.' && strlen(fn)>4 && !stricmp(fn+strlen(fn)-4,".txt"))
      {
        ds.GetCurrentFullFN(&s);
        g_scripts.Add(new scriptInstance(s.Get(),results));
      }
    }
    while (!ds.Next());
  }
}

void load_all_scripts(WDL_FastString &results)
{
  g_inputs.Empty(true);
  g_outputs.Empty(true);
  g_scripts.Empty(true);

  int x;
  for (x=0;x<g_script_load_paths.GetSize();x++)
  {
    results.AppendFormatted(512,"===== Loading scripts from %s:\r\n\r\n",g_script_load_paths.Get(x));
    load_scripts_for_path(g_script_load_paths.Get(x),results);
  }
  for (x=0;x<g_script_load_filenames.GetSize();x++)
    g_scripts.Add(new scriptInstance(g_script_load_filenames.Get(x),results));

  results.AppendFormatted(512,"Total: %d scripts, %d inputs, %d outputs\r\n", g_scripts.GetSize(), g_inputs.GetSize(),g_outputs.GetSize());

  results.Append("\r\n");
  for (x=0;x<80;x++) results.Append("=");
  results.Append("\r\n");


  for (x=0;x<g_scripts.GetSize(); x++) g_scripts.Get(x)->start(results);

  for (x=0;x<g_inputs.GetSize();x++)
  {
    inputDevice *rec=g_inputs.Get(x);
    if (rec) rec->start();
  }

}

WDL_DLGRET mainProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  static WDL_WndSizer resize;
  static WDL_FastString results;
  switch (uMsg)
  {
    case WM_INITDIALOG:
      // lParam = config file
      g_hwnd=hwndDlg;
      resize.init(hwndDlg);
      resize.init_item(IDC_EDIT1,0,0,1,1);
      resize.init_item(IDCANCEL,0,1,0,1);
      resize.init_item(IDC_LASTMSG,0,1,1,1);
      resize.init_item(IDC_CHECK1,1,1,1,1);
      resize.init_item(IDC_BUTTON1,1,1,1,1);

      g_last_wndpos.left = GetPrivateProfileInt("oscii-bot", "wnd_x",0,g_ini_file.Get());
      g_last_wndpos.top = GetPrivateProfileInt("oscii-bot", "wnd_y",0,g_ini_file.Get());
      g_last_wndpos.right = GetPrivateProfileInt("oscii-bot", "wnd_w",0,g_ini_file.Get());
      g_last_wndpos.bottom = GetPrivateProfileInt("oscii-bot", "wnd_h",0,g_ini_file.Get());

      {
#ifdef _WIN32
        HFONT font = CreateFont(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Courier New");
#else
        HFONT font = CreateFont(10, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Courier New");
#endif
        if (font) SendDlgItemMessage(hwndDlg,IDC_EDIT1,WM_SETFONT,(WPARAM)font,0);
      }


      if (g_last_wndpos.right > 0 && g_last_wndpos.bottom != 0)
      {
        g_last_wndpos.right += g_last_wndpos.left;
        g_last_wndpos.bottom += g_last_wndpos.top;
        SetWindowPos(hwndDlg,NULL,g_last_wndpos.left,g_last_wndpos.top,g_last_wndpos.right-g_last_wndpos.left,g_last_wndpos.bottom-g_last_wndpos.top,SWP_NOZORDER|SWP_NOACTIVATE);
      }
#ifdef __APPLE__
      ShowWindow(GetDlgItem(hwndDlg,IDCANCEL),SW_HIDE);
      {
        WDL_WndSizer__rec *r1=resize.get_item(IDCANCEL);
        WDL_WndSizer__rec *r2=resize.get_item(IDC_LASTMSG);
        if (r2 && r1)
        {
          r2->orig.left = r1->orig.left;
          resize.onResize();
        }
      }
#endif

      {
#ifdef _WIN32
        HICON icon=LoadIcon(g_hInstance,MAKEINTRESOURCE(IDI_ICON1));
        SetClassLong(hwndDlg,GCL_HICON,(LPARAM)icon);
        systray_add(hwndDlg, 0, (HICON)icon, "Cockos OSCII-bot");
#endif

        SendMessage(hwndDlg,WM_COMMAND,IDC_BUTTON1,0);
        SetTimer(hwndDlg,1,10,NULL);
        CheckDlgButton(hwndDlg,IDC_CHECK1,BST_CHECKED);
      }
    return 1;
    case WM_CLOSE:
#ifdef _WIN32
       ShowWindow(hwndDlg,SW_HIDE);
#else
       DestroyWindow(hwndDlg);
#endif
    return 1;
    case WM_DESTROY:
      if (g_last_wndpos.right > 0 && g_last_wndpos.bottom != 0)
      {
        char tmp[32];
        sprintf(tmp,"%d",g_last_wndpos.left);
        WritePrivateProfileString("oscii-bot","wnd_x", tmp, g_ini_file.Get());
        sprintf(tmp,"%d",g_last_wndpos.top);
        WritePrivateProfileString("oscii-bot","wnd_y", tmp, g_ini_file.Get());
        sprintf(tmp,"%d",g_last_wndpos.right-g_last_wndpos.left);
        WritePrivateProfileString("oscii-bot","wnd_w", tmp, g_ini_file.Get());
        sprintf(tmp,"%d",g_last_wndpos.bottom-g_last_wndpos.top);
        WritePrivateProfileString("oscii-bot","wnd_h", tmp, g_ini_file.Get());
      }
      g_hwnd=NULL;
#ifndef _WIN32
      SWELL_PostQuitMessage(0);
#endif
    break;
    case WM_GETMINMAXINFO:
      if (lParam)
      {
        ((MINMAXINFO*)lParam)->ptMinTrackSize.x = 300;
        ((MINMAXINFO*)lParam)->ptMinTrackSize.y = 160;
      }

    return 1;
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED)
        resize.onResize();
    case WM_MOVE:
      {
      #ifdef _WIN32
          if (!IsIconic(hwndDlg) && !IsZoomed(hwndDlg))
      #endif
            GetWindowRect(hwndDlg,&g_last_wndpos);
      }
    break;

    case WM_TIMER:
      if (wParam == 1)
      {
        // periodically update IDC_LASTMSG with new message(s?), if changed
        int x;
        const int asz=results.GetLength();
        for (x=0;x<g_inputs.GetSize();x++) 
        {
          g_inputs.Get(x)->run(results);
        }

        bool needUIupdate=false;

        double curtime = timeGetTime()/1000.0;
        for (x=0;x<g_scripts.GetSize();x++)
        {
          if (g_scripts.Get(x)->run(curtime,results)) needUIupdate=true;
        }

        for (x=0;x<g_outputs.GetSize();x++) g_outputs.Get(x)->run(results);  // send queued messages

        if (results.GetLength() != asz)
        {
          if (results.GetLength() > 20000)
          {
            char *buf = (char *)results.Get();
            int pos=0;
            while (pos < results.GetLength() && (results.GetLength()-pos > 14000 || buf[pos] != '\n')) pos++;
            if (++pos < results.GetLength())
            {
              const int newlen = results.GetLength()-pos;
              memmove(buf,buf+pos,newlen);
              results.SetLen(newlen);
            }
            else
            {
              results.SetLen(0);
            }
          }
          if (IsDlgButtonChecked(hwndDlg,IDC_CHECK1))
          {
            UpdateLogText(results,GetDlgItem(hwndDlg,IDC_EDIT1),true);
          }
        }

        if (needUIupdate)
        {
          static WDL_FastString s;
          s.Set("");
          int x;
          for(x=0;x<sizeof(g_recent_events)/sizeof(g_recent_events[0]);x++)
          {
            int a = g_recent_events[x];
            if (!a) break;
            s.AppendFormatted(64,"%02x:%02x:%02x ",(a>>16)&0xff,(a>>8)&0xff,a&0xff);
          }
          SetDlgItemText(hwndDlg,IDC_LASTMSG,s.Get());
        }
      }
    return 0;
    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDC_CHECK1:
          if (IsDlgButtonChecked(hwndDlg,IDC_CHECK1))
          {
            UpdateLogText(results,GetDlgItem(hwndDlg,IDC_EDIT1),true);
          }
        break;

        case IDCANCEL:
#ifdef _WIN32
          systray_del(hwndDlg,0);
          EndDialog(hwndDlg,1);
#else
          DestroyWindow(hwndDlg);
#endif
        break;
        case IDC_BUTTON1:
          {
            results.Set("");
            load_all_scripts(results);
            UpdateLogText(results,GetDlgItem(hwndDlg,IDC_EDIT1),false);
          }
        break;
      }
    return 0;
#ifdef _WIN32
    case WM_SYSTRAY:
      switch (LOWORD(lParam))
      {
        case WM_LBUTTONDBLCLK:
          ShowWindow(hwndDlg,SW_SHOW);
          SetForegroundWindow(hwndDlg);
        return 0;
      }
    return 0;
#endif
  }
  return 0;
}

void initialize()
{
  JNL::open_socketlib();

  NSEEL_init();
  NSEEL_addfunctionex("oscsend",3,(char *)_asm_generic3parm_retd,(char *)_asm_generic3parm_retd_end-(char *)_asm_generic3parm_retd,NSEEL_PProc_THIS,(void *)&scriptInstance::_send_oscevent);
  NSEEL_addfunctionex("midisend",1,(char *)_asm_generic1parm_retd,(char *)_asm_generic1parm_retd_end-(char *)_asm_generic1parm_retd,NSEEL_PProc_THIS,(void *)&scriptInstance::_send_midievent);
  NSEEL_addfunctionex("oscmatch",1,(char *)_asm_generic1parm_retd,(char *)_asm_generic1parm_retd_end-(char *)_asm_generic1parm_retd,NSEEL_PProc_THIS,(void *)&scriptInstance::_osc_match);
  NSEEL_addfunctionex("oscparm",2,(char *)_asm_generic2parm_retd,(char *)_asm_generic2parm_retd_end-(char *)_asm_generic2parm_retd,NSEEL_PProc_THIS,(void *)&scriptInstance::_osc_parm);
  NSEEL_addfunctionex("printf",1,(char *)_asm_generic1parm_retd,(char *)_asm_generic1parm_retd_end-(char *)_asm_generic1parm_retd,NSEEL_PProc_THIS,(void *)&scriptInstance::_printf);

  if (!g_script_load_filenames.GetSize() && !g_script_load_paths.GetSize()) 
  {
    g_script_load_paths.Add(strdup(g_default_script_path.Get()));
  }
}

void OnCommandLineParameter(const char *parm, int &state)
{
  switch (state)
  {
    case 0:
      if (parm[0] == '-')
      {
        if (!strcmp(parm,"-dir"))
        {
          state=1;
        }
        else
        {
          state=-1;
        }
      }
      else
      {
        if (strstr(parm,"/") || strstr(parm,"\\"))
        {
          g_script_load_filenames.Add(strdup(parm));
        }
        else
        {
          WDL_FastString s(g_default_script_path.Get());
#ifdef _WIN32
          s.Append("\\");
#else
          s.Append("/");
#endif
          s.Append(parm);
          g_script_load_filenames.Add(strdup(s.Get()));
        }
      }
    break;
    case 1:
      if (strstr(parm,"/") || strstr(parm,"\\"))
      {
        g_script_load_paths.Add(strdup(parm));
      }
      else
      {
        WDL_FastString s(g_default_script_path.Get());
#ifdef _WIN32
        s.Append("\\");
#else
        s.Append("/");
#endif
        s.Append(parm);
        g_script_load_paths.Add(strdup(s.Get()));
      }
      state=0;
      // dir
    break;
  }
}



#ifdef _WIN32

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
  g_hInstance = hInstance;

  char exepath[2048];
  exepath[0]=0;
  GetModuleFileName(NULL,exepath,sizeof(exepath));
  char *p=exepath;
  while (*p) p++;
  while (p >= exepath && *p != '\\') p--; *++p=0;
  
  if (exepath[0])
    g_default_script_path.Set(exepath,strlen(exepath)-1);

  {
    FILE *fp = NULL;
    if (g_default_script_path.Get()[0])
    {
      g_ini_file.Set(g_default_script_path.Get());
      g_ini_file.Append("\\oscii-bot.ini");
      fp = fopen(g_ini_file.Get(),"rb");
    }

    if (!fp)
    {
      HKEY k;
      if (RegOpenKeyEx(HKEY_CURRENT_USER,"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",0,KEY_READ,&k) == ERROR_SUCCESS)
      {
        char buf[1024];
        DWORD b=sizeof(buf);
        DWORD t=REG_SZ;
        if (RegQueryValueEx(k,"AppData",0,&t,(unsigned char *)buf,&b) == ERROR_SUCCESS && t == REG_SZ)
        {
          g_default_script_path.Set(buf);
          g_default_script_path.Append("\\oscii-bot");
          CreateDirectory(g_default_script_path.Get(),NULL);

          g_ini_file.Set(g_default_script_path.Get());
          g_ini_file.Append("\\oscii-bot.ini");
        }
        RegCloseKey(k);
      }
    }
    else
    {
      fclose(fp);
    }
  }

  {
    int state=0;
    char *p=lpCmdLine;
    while (*p)
    {
      char parm[2048];
      int parm_pos=0,qs=0;

      while (isspace(*p)) p++;
      if (!*p) break;

      while (*p && (!isspace(*p) || qs))
      {
        if (*p == '\"') qs=!qs;
        else if (parm_pos < (int)sizeof(parm)-1) parm[parm_pos++]=*p;       
        p++;
      }
      parm[parm_pos]=0;

      if (!state) if (parm[0] == '/') parm[0]='-';

      OnCommandLineParameter(parm,state);

      if (state < 0) break;
    }
    if (state)
    {
      MessageBox(NULL,
        "Usage:\r\n"
        "OSCII-bot [scriptfilename.txt ...] [-dir scriptwithtxtfiles]\r\n\r\n"
        "If no script files or paths specified, default will be all txt files in AppData/Roaming/OSCII-bot/","Usage",MB_OK);
      return 0;
    }
  }

  initialize();

  DialogBox(hInstance,MAKEINTRESOURCE(IDD_DIALOG1), GetDesktopWindow(), mainProc);

  g_inputs.Empty(true);
  g_outputs.Empty(true);
  g_scripts.Empty(true);

  ExitProcess(0);
  
  return 0;
}

BOOL systray_add(HWND hwnd, UINT uID, HICON hIcon, LPSTR lpszTip)
{
  NOTIFYICONDATA tnid;
  tnid.cbSize = sizeof(NOTIFYICONDATA);
  tnid.hWnd = hwnd;
  tnid.uID = uID;
  tnid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
  tnid.uCallbackMessage = WM_SYSTRAY;
  tnid.hIcon = hIcon;
  lstrcpyn(tnid.szTip,lpszTip,sizeof(tnid.szTip)-1);
  return (Shell_NotifyIcon(NIM_ADD, &tnid));
}

BOOL systray_del(HWND hwnd, UINT uID) {
  NOTIFYICONDATA tnid;
  tnid.cbSize = sizeof(NOTIFYICONDATA);
  tnid.hWnd = hwnd;
  tnid.uID = uID;
  return(Shell_NotifyIcon(NIM_DELETE, &tnid));
}
#endif


#ifndef _WIN32
extern "C" {

char **g_argv;
int g_argc;

};

INT_PTR SWELLAppMain(int msg, INT_PTR parm1, INT_PTR parm2)
{
  switch (msg)
  {
    case SWELLAPP_ONLOAD:
      {
        char exepath[2048];
        exepath[0]=0;
        GetModuleFileName(NULL,exepath,sizeof(exepath));
        char *p=exepath;
        while (*p) p++;
        while (p >= exepath && *p != '/') p--; *++p=0;
        
        if (exepath[0])
          g_default_script_path.Set(exepath,strlen(exepath)-1);
        
        FILE *fp=NULL;
        if (g_default_script_path.Get()[0])
        {
          g_ini_file.Set(g_default_script_path.Get());
          g_ini_file.Append("/oscii-bot.ini");
          fp = fopen(g_ini_file.Get(),"r");
        }
        if (!fp)
        {
          char *p=getenv("HOME");
          if (p && *p)
          {
            g_default_script_path.Set(p);
            g_default_script_path.Append("/Library/Application Support/OSCII-bot");
            mkdir(g_default_script_path.Get(),0777);
            g_ini_file.Set(g_default_script_path.Get());
            g_ini_file.Append("/oscii-bot.ini");
          }
        }
        else
        {
          fclose(fp);
        }
        
        if (g_argc && g_argv)
        {
          int x;
          int state=0;
          for(x=1;x<g_argc;x++)
          {
            OnCommandLineParameter(g_argv[x],state);
          }
          if (state)
          {
            printf(
            "Usage:\n"
            "%s [scriptfilename.txt ...] [-dir scriptwithtxtfiles]\n\n"
            "If no script files specified, default will be all txt files in ~/Library/Application Support/OSCII-bot",g_argv[0]);
          }
        }
        initialize();
      }
    break;
    case SWELLAPP_LOADED:
      {
        HWND h=CreateDialog(NULL,MAKEINTRESOURCE(IDD_DIALOG1),NULL,mainProc);
        ShowWindow(h,SW_SHOW);
      }
    break;
    case SWELLAPP_DESTROY:
      if (g_hwnd) DestroyWindow(g_hwnd);
      g_inputs.Empty(true);
      g_outputs.Empty(true);
      g_scripts.Empty(true);
    break;
  }
  return 0;
}



#include "../WDL/swell/swell-dlggen.h"
#include "res.rc_mac_dlg"
#undef BEGIN
#undef END
#include "../WDL/swell/swell-menugen.h"
#include "res.rc_mac_menu"

#endif
