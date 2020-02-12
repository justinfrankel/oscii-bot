// OSCII-bot
// Copyright (C) 2014 and onward Cockos Incorporated
// License: GPL

#define OSCIIBOT_VERSION "0.7"
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
#include "../WDL/assocarray.h"
#include "../WDL/mutex.h"
#include "../WDL/lineparse.h"
#include "../WDL/wingui/wndsize.h"

#include "device.h"
#include "oscmsg.h"

#define EEL_WANT_DOCUMENTATION
#include "../WDL/eel2/ns-eel-func-ref.h"

#if defined(_MSC_VER) && defined(strcasecmp)
#undef strcasecmp
#endif

#include "../WDL/jnetlib/jnetlib.h"

HINSTANCE g_hInstance;

#ifdef _WIN32
#define WM_SYSTRAY              WM_USER + 0x200
BOOL systray_add(HWND hwnd, UINT uID, HICON hIcon, LPSTR lpszTip);
BOOL systray_del(HWND hwnd, UINT uID);

static void GetShellPath(WDL_FastString *so, const char *name)
{
  HKEY k;
  if (RegOpenKeyEx(HKEY_CURRENT_USER,"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",0,KEY_READ,&k) == ERROR_SUCCESS)
  {
    char path[4096];
    DWORD b=sizeof(path);
    DWORD t=REG_SZ;
    path[0]=0;
    if (RegQueryValueEx(k,name,0,&t,(unsigned char *)path,&b) == ERROR_SUCCESS && t == REG_SZ)
    {
      so->Set(path);
    }
    RegCloseKey(k);
  }
}

#endif


int g_recent_events[4];
const char *g_code_names[4] = { "@init", "@timer", "@midimsg", "@oscmsg" };
bool g_force_results_update;

HWND g_hwnd;
RECT g_last_wndpos;
WDL_FastString g_ini_file;
WDL_FastString g_default_script_path;
WDL_PtrList<char> g_script_load_filenames, g_script_load_paths;

class eel_string_context_state;
class eel_lice_state;
class eel_net_state;

class scriptInstance 
{
  public:
  
  enum {
    MAX_OSC_FMTS=32,
    MAX_FILE_HANDLES=128,
    OSC_CURMSG_STRING=8000,
    MIDI_CURMSG_STRING=8001,
    
    DEVICE_INDEX_BASE =0x400000,
    FILE_HANDLE_INDEX_BASE=0x600000
  };
  
    scriptInstance(const char *fn, WDL_FastString &results);
    ~scriptInstance() ;

    void init_vm();
    void load_script(WDL_FastString &results);
    void import_script(const char *fn, const char *callerfn, WDL_FastString &results);
    void clear();

    void start(WDL_FastString &results);


    void WriteOutput(const char *buf)
    {
      if (m_debugOut && buf[0])
      {
        const int oldlen = m_debugOut->GetLength() ;
        const char *str = m_debugOut->Get();
        if (oldlen>0 && str[oldlen-1] == '\r') // chintzy terminal emulation
        {
          const char *p= str+ oldlen-2;
          while (p >= str && *p != '\n') p--;
          m_debugOut->SetLen((int) (p-str + 1));
        }

        while (buf[0])
        {
          if (buf[0] == 27 && buf[1] == '[' && buf[2] == '2' && buf[3] == 'J')
          {
            m_debugOut->Set("");
            buf += 4;
          }
          else
          {
#ifdef _WIN32
            // convert \r\n, or \n, into \r\n
            if (buf[0] == '\n' || (buf[0] == '\r' && buf[1] == '\n'))
            {
               m_debugOut->Append("\r\n",2);
               buf += buf[0] == '\n' ? 1 : 2;
            }
            else
#endif
            {
              m_debugOut->Append(buf++,1);
            }
          }
        }

        g_force_results_update=true;
      }
    }

    FILE *m_handles[MAX_FILE_HANDLES];
    
    void translateFilenamePath(const char *fn, WDL_FastString *fsOut)
    {
      if (!strncmp(fn,"~/",2))
      {
#ifdef _WIN32
        // win32 "home" sucks, so just go to desktop
        GetShellPath(fsOut,"Desktop");
        if (fsOut->Get()[0]) { fsOut->Append("\\"); fsOut->Append(fn+2); }
#else
        char *h = getenv("HOME");
        if (h && *h) { fsOut->Set(h); fsOut->Append("/"); fsOut->Append(fn+2); }
#endif
      }
      else if (!strncmp(fn,"@/",2))
      {
#ifdef _WIN32
        GetShellPath(fsOut,"Desktop");
        if (fsOut->Get()[0]) { fsOut->Append("\\"); fsOut->Append(fn+2); }
#else
        char *h = getenv("HOME");
        if (h && *h) { fsOut->Set(h); fsOut->Append("/Desktop/"); fsOut->Append(fn+2); }
#endif
      }
      else if (!strncmp(fn,"^/",2))
      {
        fsOut->Set(m_fn.Get());
        int idx=fsOut->GetLength();
        while (idx>=0 && fsOut->Get()[idx] != '\\' && fsOut->Get()[idx] != '/') idx--;
        if (idx>=0)
        {
          fsOut->SetLen(idx+1);
          fsOut->Append(fn+2);
        }
      }
    }


    EEL_F OpenFile(const char *fn, const char *mode)
    {
      if (!*fn || !*mode) return 0.0;

      int x;
      for (x=0;x<MAX_FILE_HANDLES && m_handles[x];x++);
      if (x>= MAX_FILE_HANDLES) 
      {
        DebugOutput("fopen(): no free file handles");
        return 0.0;
      }

      WDL_FastString tmp;
      translateFilenamePath(fn,&tmp);

      FILE *fp = fopen(tmp.Get()[0] ? tmp.Get():fn,mode);
      if (!fp) 
      {
        DebugOutput("fopen(): Failed opening file %s(\"%s\")",tmp.Get(),mode);
        return 0.0;
      }
      m_handles[x]=fp;
      return x + FILE_HANDLE_INDEX_BASE;
    }
    EEL_F CloseFile(int fp_idx)
    {
      fp_idx-=FILE_HANDLE_INDEX_BASE;
      if (fp_idx>=0 && fp_idx<MAX_FILE_HANDLES && m_handles[fp_idx])
      {
        fclose(m_handles[fp_idx]);
        m_handles[fp_idx]=0;
        return 0.0;
      }
      return -1.0;
    }
    FILE *GetFileFP(int fp_idx)
    {
      fp_idx-=FILE_HANDLE_INDEX_BASE;
      if (fp_idx>=0 && fp_idx<MAX_FILE_HANDLES) return m_handles[fp_idx];
      return NULL;
    }


    void compileCode(int parsestate, const WDL_FastString &curblock, WDL_FastString &results, int lineoffs);
    bool run(double curtime, WDL_FastString &results);
    static void messageCallback(void *d1, void *d2, char type, int msglen, void *msg);

    WDL_FastString m_fn;
    WDL_FastString *m_debugOut;

    struct incomingEvent
    {
      EEL_F *dev_ptr;
      int sz; // size of msg[], 1..3 for midi, anything for OSC
      char type; // 0=midi, 1=OSC
      unsigned char msg[3];
    };


    // these are non-owned refs
    WDL_PtrList<ioDevice> m_devs;

    WDL_HeapBuf m_incoming_events;  // incomingEvent list, each is 8-byte aligned
    WDL_Mutex m_incoming_events_mutex;

    eel_string_context_state *m_eel_string_state;
    eel_lice_state *m_lice_state;
    eel_net_state *m_net_state;
    
    void DebugOutput(const char *fmt, ...)
    {
      va_list arglist;
      va_start(arglist, fmt);
      if (m_debugOut)
      {
        m_debugOut->SetAppendFormattedArgs(true,512,fmt,arglist);
        m_debugOut->Append("\r\n");
      }
      va_end(arglist);
    }

    const char *GetStringForIndex(EEL_F val, WDL_FastString **isWriteableAs=NULL, bool is_for_write=false);
    EEL_F *GetVarForFormat(int formatidx);

    bool GetImageFilename(EEL_F val, WDL_FastString *fsWithDir, int isWrite)
    {
      const char *fn=GetStringForIndex(val,NULL);
      if (fn)
      {
        fsWithDir->Set(fn);
        translateFilenamePath(fn,fsWithDir);
        if (!fsWithDir->Get()[0]) fsWithDir->Set(fn);
        return true;
      }

      return false;
    }


    EEL_F *m_var_time, *m_var_msgs[5], *m_var_fmt[MAX_OSC_FMTS];
    NSEEL_VMCTX m_vm;
    NSEEL_CODEHANDLE m_code[4]; // init, timer, message code, oscmsg code
    WDL_PtrList<void> m_imported_code; // NSEEL_CODEHANDLE, for @import etc

    WDL_StringKeyedArray<bool> m_loaded_fnlist; // @import-ed file list

    const OscMessageRead *m_cur_oscmsg;
    WDL_FastString m_cur_midi_sysex_msg;
 
    struct evalCacheEnt {
      char *str;
      NSEEL_CODEHANDLE ch;
    };
    WDL_TypedBuf<evalCacheEnt> m_eval_cache;
    char *evalCacheGet(const char *str, NSEEL_CODEHANDLE *ch);
    void evalCacheDispose(char *key, NSEEL_CODEHANDLE ch);
    
    static EEL_F NSEEL_CGEN_CALL _send_oscevent(void *opaque, INT_PTR np, EEL_F **parms);
    static EEL_F NSEEL_CGEN_CALL _send_midievent_str(void *opaque, EEL_F *dest_device, EEL_F *strptr);
    static EEL_F NSEEL_CGEN_CALL _send_midievent(void *opaque, EEL_F *dest_device);
    static EEL_F NSEEL_CGEN_CALL _get_device_open_time(void *opaque, EEL_F *dest_device);
    static EEL_F NSEEL_CGEN_CALL _osc_parm(void *opaque, INT_PTR np, EEL_F **parms);
    static EEL_F NSEEL_CGEN_CALL _osc_match(void *opaque, INT_PTR np, EEL_F **parms);
};

#define EEL_STRINGS_MUTABLE_LITERALS // OSCII-bot has always had mutable strings, sooo....
#define EEL_STRING_GET_CONTEXT_POINTER(opaque) (((scriptInstance *)opaque)->m_eel_string_state)

// override defaults for format variables
#define EEL_STRING_GETFMTVAR(x) ((scriptInstance*)(opaque))->GetVarForFormat(x)
// override defaults for OSC index of 8000
#define EEL_STRING_GET_FOR_INDEX(x, wr) ((scriptInstance*)(opaque))->GetStringForIndex(x, wr)
#define EEL_STRING_GET_FOR_WRITE(x, wr) ((scriptInstance*)(opaque))->GetStringForIndex(x, wr,true)

// debug/stdout write
#define EEL_STRING_DEBUGOUT ((scriptInstance*)(opaque))->DebugOutput // no parameters, since it takes varargs
#define EEL_STRING_STDOUT_WRITE(x,len) ((scriptInstance*)(opaque))->WriteOutput(x) 

#include "../WDL/eel2/eel_strings.h"

#define EEL_FILE_OPEN(fn,mode) ((scriptInstance*)opaque)->OpenFile(fn,mode)
#define EEL_FILE_GETFP(fp) ((scriptInstance*)opaque)->GetFileFP(fp)
#define EEL_FILE_CLOSE(fpindex) ((scriptInstance*)opaque)->CloseFile(fpindex)

#include "../WDL/eel2/eel_files.h"

#define EEL_LICE_STANDALONE_PARENT(opaque) (g_hwnd)
#define EEL_LICE_GET_FILENAME_FOR_STRING(idx, fs, p) (((scriptInstance*)opaque)->GetImageFilename(idx,fs,p))
#define EEL_LICE_GET_CONTEXT_INIT(x) (((scriptInstance *)opaque)->m_lice_state)
#define EEL_LICE_GET_CONTEXT(x) (EEL_LICE_GET_CONTEXT_INIT(x) && EEL_LICE_GET_CONTEXT_INIT(x)->hwnd_standalone ? EEL_LICE_GET_CONTEXT_INIT(x) : NULL)
#define EEL_LICE_WANT_STANDALONE

#include "../WDL/eel2/eel_lice.h"

#include "../WDL/eel2/eel_misc.h"

#define EEL_NET_GET_CONTEXT(opaque) (((scriptInstance *)opaque)->m_net_state)
#include "../WDL/eel2/eel_net.h"

#define EEL_EVAL_GET_CACHED(str, ch) ((scriptInstance *)opaque)->evalCacheGet(str,&(ch))
#define EEL_EVAL_SET_CACHED(str, ch) ((scriptInstance *)opaque)->evalCacheDispose(str,ch)
#define EEL_EVAL_GET_VMCTX(opaque) (((scriptInstance *)opaque)->m_vm)

#include "../WDL/eel2/eel_eval.h"

double get_time_precise()
{
  EEL_F v=0.0;
  _eel_time_precise(NULL,&v);
  return v;
}

static bool IsFuncChar(char c)
{
  return (isalpha(c) || isdigit(c) || c == '_');
}

static void writeWithHTMLEntities(FILE *fp, const char *rd, int rdsz, WDL_PtrList<const char> *doseechk)
{
  while (rdsz-- > 0 && *rd)
  {
    if (doseechk && !strnicmp(rd,"see ",4))
    {
      const char* p=rd+4;
      int len=0;
      while (IsFuncChar(p[len])) ++len;
      if (len > 0)
      {
        int x;
        for(x=0;x<doseechk->GetSize();x++)
        {
          if (!strnicmp(doseechk->Get(x),p,len) && doseechk->Get(x)[len]=='\t')
          {
            fprintf(fp,"see <a href=\"#%.*s\">%.*s</a>",len,p,len,p);
            break;
          }
        }
        if (x < doseechk->GetSize())
        {
          rd+=len+4;
          rdsz-=len+4;
          rdsz++;
          continue;
        }
      }
    }
    const char c = *rd++;
    if (c == 2) fprintf(fp,"</ul>");
    else if (c == 3) fprintf(fp,"<ul>");
    else if (c == 4) fprintf(fp,"<li>");
    else if (c == '\n') fprintf(fp,"<br>");
    else if (c == '&') fprintf(fp, "&amp;");
    else if (c == '<') fprintf(fp, "&lt;");
    else if (c == '>') fprintf(fp, "&gt;");
    else fputc(c, fp);
  }
}

void doc_Generate()
{
  char fn[1024];
  GetTempPath(512, fn);
  strcat(fn,"oscii-bot-doc.html");
  FILE *fp = fopen(fn,"wb");
  if (!fp) 
  {
    char buf[4096];
    snprintf(buf,sizeof(buf),"Error writing: %s",fn);
    MessageBox(g_hwnd,buf,"Error writing documentation",MB_OK);
    return;
  }

  fprintf(fp,
      "<html><head><title>\n"
      "OSCII-bot code reference\n"
      "</title></head><body>\n");

  fprintf(fp, "<p align=\"left\"><h1>OSCII-bot code reference</h1></p>\n");
  fprintf(fp, "<p align=\"right\"><font size=1>Generated by OSCII-bot v%s</font></p>\n", OSCIIBOT_VERSION);

  fprintf(fp,"Scripts use lines beginning with @ to specify various parameters:<ul>\n");
  fprintf(fp,"<li>@input : specifies a device to open for input. Usage:<ul>\n"
                "<li>@input devicehandle MIDI \"substring devicename match\" [skip_count]\n"
                "<li>@input devicehandle OSC \"1.2.3.4:port\"\n"
                "<li>@input devicehandle OSC \"*:port\"\n"
                "<li>@input devicehandle OMNI-MIDI<i> -- receives all MIDI received by other scripts</i>\n"
                "<li>@input devicehandle OMNI-OSC<i> -- receives all OSC received by other scripts</i>\n"
                "<li>@input devicehandle OMNI-MIDI-OUTPUT<i> -- receives all MIDI sent by other scripts</i>\n"
                "<li>@input devicehandle OMNI-OSC-OUTPUT<i> -- receives all OSC sent by other scripts</i>\n"
                "</ul>\n"
                "Note: in OSCII-bot v0.4+, you can send OSC messages to an OSC input device, which will send to the IP/port of the last message received.\n"
                );

  fprintf(fp,"<li>@output : specifies a device to open for output. Usage:<ul>\n"
                "<li>@output devicehandle OSC \"host:port\" [maxpacketsize (def=1024)] [sleepinMS (def=10)]\n"
                "<li>@output devicehandle MIDI \"substring match\" [skip]\n"
                "</ul>\n"
                "Note: in OSCII-bot v0.4+, you may also receive messages from an OSC output (if the other end replies).\n"
                );


  fprintf(fp,"<li>@init : begins code that is executed on script load/recompile.\n");
  fprintf(fp,"<li>@timer : begins code that is executed periodically, usually around 100 times per second.\n");
  fprintf(fp,"<li>@midimsg : begins code that is executed on receipt of a MIDI message. In this case, msg1/msg2/msg3 will be set to the parameters of the MIDI message, and msgdev will receive the MIDI device index. In OSCII-bot v0.5+, if a SysEx message is received, msg1/msg2/msg3 will all be 0, and oscstr will be set to a string with the SysEx data.\n");
  fprintf(fp,"<li>@oscmsg : begins code that is executed on receipt of an OSC message. In this case, msgdev will specify the device, oscstr will be set to a string that specifies the OSC message, and see oscparm() to query the values of the OSC parameters. To quickly match the OSC message against various strings, see match() or see oscmatch().\n");
  fprintf(fp,"<li>@import : import functions from other reascripts using <code>@import filename.txt</code> -- note that only the file's functions will be imported, normal code in that file will not be executed.\n");
  fprintf(fp,"</ul>");


  fprintf(fp,"Special variables:<ul>\n"
      "<li>msg1/msg2/msg3: used to specify MIDI message values received by @midimsg or sent (see midisend())\n"
      "<li>msgdev: specifies the device on receipt of a MIDI or OSC message in @midimsg or @oscmsg\n"
      "<li>oscstr: specifies a string of a received OSC message in @oscmsg, or (OSCII-bot v0.5+) of a SysEx message in @midimsg. Will be set to -1 if not valid.\n"
      "<li>fmt0..fmt31: specifies (deprecated) format values for various functions including sprintf(), match(), oscmatch(), etc. \n"
      "<li>time: set to a timestamp in seconds with at least millisecond granularity\n"
      "</ul>\n\n");

  // code reference

  fprintf(fp,"The code for OSCII-bot is written in a simple language (called EEL2), which has many similarities to C. Code is written in one or more of the code sections listed above. Some basic features of this language are:<ul> \n"
      "<li>Variables do not need to be declared, are by default global, and are all double-precision floating point, or strings if a # prefix is specified\n"
      "<li>Basic operations including addition (+), subtraction (-), multiplication (*), division (/), and exponential (^)\n"
      "<li>Bitwise operations including OR (|), AND (&amp;), XOR (~), shift-left (&lt;&lt;),  and shift-right-sign-extend (&gt;&gt;). These all convert to integer for calculation.\n"
      "<li>Parentheses '(' and ')' can be used to clarify precidence, contain parameters for functions, and collect multiple statements into a single statement.\n"
      "<li>A semicolon ';' is used to separate statements from eachother (including within parentheses).\n"
      "<li>A virtual local address space of about 8 million words, which can be accessed via brackets '[' and ']'. \n"
      "<li>A shared global address space of about 1 million words, accessed via gmem[]. These words are shared between all OSCII-bot scripts.\n"
      "<li>Shared global named variables, accessible via the '_global.' prefix. These variables are shared between all OSCII-bot scripts.\n"
      "<li><a href=\"http://www.reaper.fm/sdk/js/userfunc.php\">User definable functions</a>, which can define private variables, parameters, and also can optionally access namespaced instance variables.\n"
      "<li>Numbers are in normal decimal, however if you prefix $x or 0x to them, they will be hexadecimal (i.e. $x90, 0xDEADBEEF, etc)\n"
      "<li>You may specify the ASCII value of a character using $'c' or 'c' (where c is the character). You can also specify multibyte characters using 'xy'\n"
      "<li>If you wish to generate a mask of 1 bits in integer, you can use $~X, for example $~7 is 127, $~8 is 255, $~16 is 65535, etc.\n"
      "<li>Comments can be specified using:<ul> \n"
      "<li>// comments to end of line\n"
      "<li>/* comments block of code that span lines or be part of a line */\n"
     "</ul> \n"
   "</ul> \n"
  "<hr><h2>Operator reference</h2>\n"
  "Listed from highest precedence to lowest (but one should use parentheses whenever there is doubt!):<ul> \n"
  "<li><a name=\"op_ram\"></a><b>  [ ]</b><pre>\n"
  "  z=x[y];\n"
  "  x[y]=z;\n"
  "</pre>\n"
  "You may use brackets to index into memory that is local to your script. Your script has approximately 8 million (8,388,608) slots of memory and you may access them either with fixed offsets (i.e. 16811[0]) or with variables (myBuffer[5]). The sum of the value to the left of the brackets and the value within the brackets is used to index memory. If a value in the brackets is omitted then only the value to the left of the brackets is used.<pre>\n"
  "  z=gmem[y];\n"
  "  gmem[y]=z;\n"
  "</pre>\n"
  "<a name=\"op_gmem\"></a>If 'gmem' is specified as the left parameter to the brackets, then the global shared buffer is used, which is approximately 1 million (1,048,576) slots that are shared across all scripts\n"
  "<BR><BR>\n"
  "<li><a name=\"op_not\"></a><b>!value</b> -- returns the logical NOT of the parameter (if the parameter is 0.0, returns 1.0, otherwise returns 0.0).\n"
  "<li><a name=\"op_neg\"></a><b>-value</b> -- returns value with a reversed sign (-1 * value).\n"
  "<li><a name=\"op_pos\"></a><b>+value</b> -- returns value unmodified. \n"
  "<BR><BR>\n"
  "<li><a name=\"op_pow\"></a><b>base ^ exponent</b> -- returns the first parameter raised to the power of the second parameter. This is also available the function pow(x,y) <BR><BR>\n"
  "<li><a name=\"op_mod\"></a><b>numerator %% denominator</b> -- divides two values as integers and returns the remainder.\n"
  "<BR><BR>\n"
  "<li><a name=\"op_shl\"><b>value &lt;&lt; shift_amt</b> -- converts both values to 32 bit integers, bitwise left shifts the first value by the second. Note that shifts by more than 32 or less than 0 produce undefined results.\n"
  "<BR><BR>\n"
  "<li><a name=\"op_shr\"><b>value &gt;&gt; shift_amt</b> -- converts both values to 32 bit integers, bitwise right shifts the first value by the second, with sign-extension (negative values of y produce non-positive results). Note that shifts by more than 32 or less than 0 produce undefined results.\n"
  "<BR><BR>\n"
  "<li><a name=\"op_div\"></a><b>value / divisor</b> -- divides two values and returns the quotient.\n"
  "<BR><BR>\n"
  "<li><a name=\"op_mul\"></a><b>value * another_value</b> -- multiplies two values and returns the product.\n"
  "<BR><BR>\n"
  "<li><a name=\"op_sub\"></a><b>value - another_value</b> -- subtracts two values and returns the difference.\n"
  "<BR><BR>\n"
  "<li><a name=\"op_add\"></a><b>value + another_value</b> -- adds two values and returns the sum.\n"
  "<BR><BR>\n"
  "<li><a name=\"op_bor\"></a><b>a | b</b> -- converts both values to integer, and returns bitwise OR of values.\n"
  "<li><a name=\"op_band\"></a><b>a &amp b</b> -- converts both values to integer, and returns bitwise AND of values.\n"
  "<li><a name=\"op_xor\"><b>a ~ b</b> -- converts both values to 32 bit integers, bitwise XOR the values.\n"
  "<BR><BR>\n"
  "<li><a name=\"op_cmp\"></a><b>value1 == value2</b> -- compares two values, returns 1 if difference is less than 0.00001, 0 if not.\n"
  "<li><b>value1 === value2</b> -- compares two values, returns 1 if exactly equal, 0 if not.\n"
  "<li><b>value1 != value2</b> -- compares two values, returns 0 if difference is less than 0.00001, 1 if not.\n"
  "<li><b>value1 !== value2</b> -- compares two values, returns 0 if exactly equal, 1 if not.\n"
  "<li><b>value1 &lt; value2</b> -- compares two values, returns 1 if first parameter is less than second.\n"
  "<li><b>value1 &gt; value2</b> -- compares two values, returns 1 if first parameter is greater than second.\n"
  "<li><b>value1 &lt;= value2</b> -- compares two values, returns 1 if first is less than or equal to second.\n"
  "<li><b>value1 &gt;= value2</b> -- compares two values, returns 1 if first is greater than or equal to second.\n"
  "<BR><BR>\n"

  "<li><b>y || z</b> -- returns logical OR of values. If 'y' is nonzero, 'z' is not evaluated.\n"
  "<li><b>y &amp&amp z</b> -- returns logical AND of values. If 'y' is zero, 'z' is not evaluated.\n"
  "<BR><BR>\n"

  "<li><b>y ? z</b><a name=\"op_br\"></a> <i>&nbsp;&nbsp;&nbsp;&nbsp; -- how conditional branching is done -- similar to C's if/else</i><BR> \n"
  "                        <b>y ? z : x</b><BR><BR>\n"
  "    If y is non-zero, executes and returns z, otherwise executes and returns x (or 0.0 if <i>': x'</i> is not specified). \n"
  "    <BR><BR>\n"
  "    Note that the expressions used can contain multiple statements within parentheses, such as:<pre> \n"
  "      x %% 5 ? (\n"
  "        f += 1;\n"
  "        x *= 1.5;\n"
  "      ) : (\n"
  "        f=max(3,f);\n"
  "        x=0;\n"
  "      );\n"
  "</pre>\n"
  "<li><a name=\"op_ass\"></a><b>y = z</b> -- assigns the value of 'z' to 'y'. 'z' can be a variable or an expression.\n"
  "<li><a name=\"op_ass_mul\"></a><b>y *= z</b> -- multiplies two values and stores the product back into 'y'.\n"
  "<li><a name=\"op_ass_div\"></a><b>y /= divisor</b> -- divides two values and stores the quotient back into 'y'.\n"
  "<li><a name=\"op_ass_mod\"></a><b>y %%= divisor</b> -- divides two values as integers and stores the remainder back into 'y'.\n"
  "<li><a name=\"op_ass_pow\"></a><b>base ^= exponent</b> -- raises first parameter to the second parameter-th power, saves back to 'base'\n"
  "<li><a name=\"op_ass_add\"></a><b>y += z</b> -- adds two values and stores the sum back into 'y'.\n"
  "<li><a name=\"op_ass_sub\"></a><b>y -= z</b> -- subtracts 'z' from 'y' and stores the difference into 'y'.\n"
  "<li><a name=\"op_ass_or\"></a><b>y |= z</b> -- converts both values to integer, and stores the bitwise OR into 'y'\n"
  "<li><a name=\"op_ass_and\"></a><b>y &amp= z</b> -- converts both values to integer, and stores the bitwise AND into 'y'\n"
  "<li><a name=\"op_ass_xor\"></a><b>y ~= z</b> -- converts both values to integer, and stores the bitwise XOR into 'y'\n"
  "</ul> \n"
  "<BR><BR> \n"
  "Some key notes about the above, especially for C programmers:<ul> \n"
  "<li>( and ) (vs { } ) --  enclose multiple statements, and the value of that expression is the last statement within the block:\n"
  "  <pre>\n"
  "     z = (\n"
  "       a = 5; \n"
  "       b = 3; \n"
  "       a+b;\n"
  "     ); // z will be set to 8, for example\n"
  "  </pre>\n"
  "<li>Conditional branching is done using the ? or ? : operator, rather than if()/else.<pre>\n"
  "   a &lt; 5 ? b = 6; // if a is less than 5, set b to 6\n"
  "   a &lt; 5 ? b = 6 : c = 7; // if a is less than 5, set b to 6, otherwise set c to 7\n"
  "   a &lt; 5 ? ( // if a is less than 5, set b to 6 and c to 7\n"
  "     b = 6;\n"
  "     c = 7;\n"
  "   );\n"
  "</pre>\n"
  "<li>The ? and ?: operators can also be used as the lvalue of expressions:<pre>\n"
  "   (a &lt; 5 ? b : c) = 8; // if a is less than 5, set b to 8, otherwise set c to 8\n"
  "</pre>\n"
  "</ul>\n"
  "</ul>\n");
 
  fprintf(fp,
    "<hr><h2>Strings</h2>\n"
    "Strings can be specified as literals using quotes, such as \"This is a test string\". Much of the syntax mirrors that of C: you must escape quotes with backslashes to put them in strings (\"He said \\\"hello, world\\\" to me\"), multiple literal strings will be automatically concatenated by the compiler. Unlike C, quotes can span multiple lines. There is a soft limit on the size of each string: attempts to grow a string past about 16KB will result in the string not being modified.\n"
   "<BR><BR>\n"
   "Strings are always refered to by a number, so one can reference a string using a normal JS variable:<pre>\n"
   "    x = \"hello world\";\n"
   "    gfx_drawstr(x);\n"
   "</pre>\n"
   "Literal strings are mutable in OSCII-bot, and you can also have other named strings:<ul>\n"
   "<li>You can use the fixed values of 0-1023:<pre>\n"
   "   x = 50; // string slot 50\n"
   "   strcpy(x, \"hello \");\n"
   "   strcat(x, \"world\");\n"
   "   gfx_drawstr(x);\n"
   "</pre>\n"
   "This mode is useful if you need to build or load a table of strings.\n"
   "<BR><BR>\n"
   "<li>You can use # to get an instance of a temporary string:<pre>\n"
   "   x = #;\n"
   "   strcpy(x, \"hello \");\n"
   "   strcat(x, \"world\");\n"
   "   gfx_drawstr(x);\n"
   "</pre>\n"
   "Note that the scope of these temporary instances is very limited and unpredictable, and their initial values are undefined. \n"
   "<BR><BR>\n"
   "<li>Finally, you can use named strings, which are the equivalent of normal variables:<pre>\n"
   "  x = #myString;\n"
   "  strcpy(x, \"hello world\");\n"
   "</pre>\n"
   "The value of named strings is defined to be empty at script load, and to persist throughout the life of your script. There is also a shortcut to assign/append to named strings:<pre>\n"
   "  #myString = \"hello \";  // same as strcpy(#myString, \"hello \");\n"
   "  #myString += \"world\"; // same as strcat(#myString, \"world\");\n"
   "</pre>\n"
   "</ul>\n");


  WDL_PtrList<const char> _fs, *fs = &_fs;

  const char *p = nseel_builtin_function_reference;
  while (*p) { fs->Add(p); p += strlen(p) + 1; }

  _fs.Add("midisend\tdevice_index\tSends a MIDI event (specified by variables msg1,msg2,msg3) to the device specified by device_index.\n\ndevice_index can be -100 to send to all outputs opened by application, -1 to send to all outputs opened by script. ");
  _fs.Add("midisend_str\tdevice_index,\"string\"\t(v0.5+) Sends a MIDI event (specified by the contents of a string) to the device specified by device_index.\n\ndevice_index can be -100 to send to all outputs opened by application, -1 to send to all outputs opened by script. Can be used to send SysEx, for example: midisend_str(midiout, \"\\xF0\\x00\\x01\\x02\\xF7\").");
  _fs.Add("oscsend\tdevice_index,\"string\"[,value,...]\tSends an OSC event (specified by \"string\" and one or more parameters specifying values) to device specified by device_index. \n\ndevice_index can be -100 to send to all outputs opened by application, -1 to send to all outputs opened by script.\n\n\"string\" is OSC message, and can have a prefix specifying type and count of values. \n\nAdditional parameters (after values) will be used as parameters to any format specifiers in \"string\". \n\nPrefixes are one or more characters of 'f' (float), 'i' (integer), 'b' (bool), 's' (string), which specify an OSC value of that type.");
  _fs.Add("oscmatch\t\"string\"[,format-output]\tMatches the current OSC event against \"string\" (see match()), and puts any matched specifiers (%s, %d, etc) into parameters specified (or fmt0..fmtX if not specified). \n\noscmatch() is the equivalent of match(\"string\",oscstr)");
  _fs.Add("oscparm\tparm_idx[,type,#string]\tGets the parameter value for the current OSC message. \n\nIf type is specified, it will be set to the type of the parameter ('f', 's', etc). \n\nIf #string is specified and type is 's', #string will be set to the OSC parameter string.");

  _fs.Add("get_device_open_time\tdevice_index\tReturns the timestamp (similar to time_precise()) of the last time this device was opened/re-opened, can be used to detect device reconnection.");

  fprintf(fp, "<BR><BR><hr><br><h2>Function List</h2>\n");

  p = eel_strings_function_reference;
  while (*p) { fs->Add(p); p += strlen(p) + 1; }
  p = eel_misc_function_reference;
  while (*p) { fs->Add(p); p += strlen(p) + 1; }
  p = eel_eval_function_reference;
  while (*p) { fs->Add(p); p += strlen(p) + 1; }
  p = eel_net_function_reference;
  while (*p) { fs->Add(p); p += strlen(p) + 1; }
  p = eel_file_function_reference;
  while (*p) { fs->Add(p); p += strlen(p) + 1; }
  p = eel_lice_function_reference;
  while (*p) { fs->Add(p); p += strlen(p) + 1; }

  {
    const int n = _fs.GetSize();
    if (n > 0)
    {
      fprintf(fp, "<a name=\"eel_list\"></a>\n");
      fprintf(fp, "<code><table>\n");
      int j=0,i;
      for (i = 0; i < n; ++i)
      {
        const int col = i % 6;

        if (!col) 
        {
          j = i/6;
          fprintf(fp, "<tr>");
        }
        if (j < n)
        {
          const char *str = _fs.Get(j);
          const char *np = strstr(str, "\t");
          const int sz = (int) (np ? (np - str) : strlen(str));
          fprintf(fp, "<td><a href=\"#%.*s\">%.*s</a> &nbsp; &nbsp; </td>", sz, str, sz, str);
        }
        else
          fprintf(fp, "<td></td>");

        if (col == 5) fprintf(fp, "</tr>");
        fprintf(fp, "\n");

        if (!col) j+= n-(n/6)*5;
        else j += n/6;
      }
      fprintf(fp, "</table></code><br><br>\n");
    }
  }
  {
    int i;
//    fprintf(fp, "<BR><BR><hr><br><h2>Function List</h2>\n");
    for (i = 0; i < _fs.GetSize(); i++)
    {
      const char *str = _fs.Get(i);
      const char *np = strstr(str, "\t");
      const int sz = (int) (np ? (np - str) : strlen(str));
      const char *parms = np ? np + 1 : "";
      const char *info = np ? strstr(np + 1, "\t") : NULL;
      const int parms_sz = (int) (info ? (info - parms) : strlen(parms));

      fprintf(fp, "<a name=\"%.*s\"><hr></a><br>\n", sz, str);
      fprintf(fp, "<code>%.*s(", sz, str);
      if (parms) writeWithHTMLEntities(fp, parms, parms_sz,NULL);
      fprintf(fp, ")</code><BR><BR>");
      if (info)
      {
        writeWithHTMLEntities(fp, info, (int) strlen(info),&_fs);
        fprintf(fp, "<BR><BR>");
      }
    }
  }

  fclose(fp);

  ShellExecute(g_hwnd, "open", fn, "", "", 0);
}

scriptInstance::scriptInstance(const char *fn, WDL_FastString &results)  : m_loaded_fnlist(false)
{ 
  m_debugOut=0;
  m_fn.Set(fn);
  m_vm=0;
  m_cur_oscmsg=0;
  memset(m_code,0,sizeof(m_code));
  memset(m_handles,0,sizeof(m_handles));
  m_eel_string_state = new eel_string_context_state;
  m_lice_state=0;
  m_net_state=0;

  m_var_time = 0;
  memset(m_var_msgs,0,sizeof(m_var_msgs));
  memset(m_var_fmt,0,sizeof(m_var_fmt));
  
  init_vm();
  load_script(results);
}

scriptInstance::~scriptInstance() 
{
  clear();
  delete m_eel_string_state;
}

void scriptInstance::clear()
{
  m_debugOut=0;
  m_devs.Empty();
  m_eel_string_state->clear_state(true);
  int x;
  for (x=0;x<MAX_FILE_HANDLES;x++) 
  {
    if (m_handles[x]) fclose(m_handles[x]); 
    m_handles[x]=0;
  }
  for (x=0;x<(int)(sizeof(m_code)/sizeof(m_code[0])); x++)
  {
    if (m_code[x]) NSEEL_code_free(m_code[x]);
    m_code[x]=0;
  }
  for (x=0;x<m_eval_cache.GetSize();x++)
  {
    free(m_eval_cache.Get()[x].str);
    NSEEL_code_free(m_eval_cache.Get()[x].ch);
  }
  m_eval_cache.Resize(0);

  m_imported_code.Empty((void (*)(void *))NSEEL_code_free);
  m_loaded_fnlist.DeleteAll();

  NSEEL_VM_free(m_vm);
  m_vm=0;
  m_incoming_events.Resize(0,false);

  m_var_time = 0;
  memset(m_var_msgs,0,sizeof(m_var_msgs));
  memset(m_var_fmt,0,sizeof(m_var_fmt));

  delete m_lice_state;
  m_lice_state=0;
  delete m_net_state;
  m_net_state=0;
}

void scriptInstance::start(WDL_FastString &results)
{
  m_eel_string_state->update_named_vars(m_vm);
  if (m_code[0])
  {
    if (m_var_time) *m_var_time = timeGetTime()/1000.0;
    m_debugOut = &results;
    NSEEL_code_execute(m_code[0]);
    m_debugOut = NULL;
  }
}

const char *scriptInstance::GetStringForIndex(EEL_F val, WDL_FastString **isWriteableAs, bool is_for_write)
{
  const int idx = (int) (val+0.5);
  if (idx == OSC_CURMSG_STRING)
  {
    if (isWriteableAs) *isWriteableAs=NULL;
    return m_cur_oscmsg ? m_cur_oscmsg->GetMessage() : NULL;
  }
  else if (idx==MIDI_CURMSG_STRING && m_cur_midi_sysex_msg.GetLength())
  {
    if (isWriteableAs && !is_for_write) *isWriteableAs=&m_cur_midi_sysex_msg;
    return m_cur_midi_sysex_msg.Get();
  }

  return m_eel_string_state->GetStringForIndex(val,isWriteableAs,is_for_write);
}

EEL_F *scriptInstance::GetVarForFormat(int formatidx)
{
  if (formatidx>=0 && formatidx<MAX_OSC_FMTS) return m_var_fmt[formatidx];
  return m_eel_string_state->GetVarForFormat(formatidx);
}

char *scriptInstance::evalCacheGet(const char *str, NSEEL_CODEHANDLE *ch)
{
  // should mutex protect if multiple threads access this sInst context
  int x=m_eval_cache.GetSize();
  while (--x >= 0)
  {
    char *ret;
    if (!strcmp(ret=m_eval_cache.Get()[x].str, str))
    {
      *ch = m_eval_cache.Get()[x].ch;
      m_eval_cache.Delete(x);
      return ret;
    }
  }
  return NULL;
}

void scriptInstance::evalCacheDispose(char *key, NSEEL_CODEHANDLE ch)
{
  // should mutex protect if multiple threads access this sInst context
  evalCacheEnt ecc;
  ecc.str= key;
  ecc.ch = ch;
  if (m_eval_cache.GetSize() > 1024) 
  {
    NSEEL_code_free(m_eval_cache.Get()->ch);
    free(m_eval_cache.Get()->str);
    m_eval_cache.Delete(0);
  }
  m_eval_cache.Add(ecc);
}

    

WDL_PtrList<scriptInstance> g_scripts;
WDL_PtrList<ioDevice> g_devices,g_omni_inputs_fordelete; // these are owned here, scriptInstances reference them
omniInputDevice *g_input_omni_outs[2]; // midi,osc

class oscDevice : public ioDevice
{
public:
  oscDevice(const char *dest, int maxpacket, int sendsleep, struct sockaddr_in *listen_addr) 
  {
    m_has_output = m_has_input=true;
    memset(&m_sendaddr, 0, sizeof(m_sendaddr));
    m_maxpacketsz = maxpacket> 0 ? maxpacket:1024;
    m_sendsleep = sendsleep >= 0 ? sendsleep : 10;

    m_sendsock=socket(AF_INET, SOCK_DGRAM, 0);

    if (m_sendsock == INVALID_SOCKET)
    {
    }
    else if (listen_addr)
    {
      m_recvaddr = *listen_addr;
      int on=1;
      setsockopt(m_sendsock, SOL_SOCKET, SO_BROADCAST, (char*)&on, sizeof(on));
      if (!bind(m_sendsock, (struct sockaddr*)&m_recvaddr, sizeof(struct sockaddr))) 
      {
        SET_SOCK_BLOCK(m_sendsock, false);
      }
      else
      {
        closesocket(m_sendsock);
        m_sendsock=INVALID_SOCKET;
      }
    }
    else
    {
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

      int on=1;
      setsockopt(m_sendsock, SOL_SOCKET, SO_BROADCAST, (char*)&on, sizeof(on));
      SET_SOCK_BLOCK(m_sendsock, false);
    }
  }


  virtual ~oscDevice() 
  { 
    if (m_sendsock != INVALID_SOCKET)
    {
      shutdown(m_sendsock, SHUT_RDWR);
      closesocket(m_sendsock);
      m_sendsock=INVALID_SOCKET;
    }
  } 

  virtual void run_input(WDL_FastString &textOut)
  {
    if (m_sendsock == INVALID_SOCKET) return;
    struct sockaddr *p = m_dest.GetLength() ? NULL : (struct sockaddr *)&m_sendaddr;
    for (;;)
    {
      char buf[16384];
      buf[0]=0;
      socklen_t plen = (socklen_t) sizeof(m_sendaddr);
      const int len=(int)recvfrom(m_sendsock, buf, sizeof(buf), 0, p, p?&plen:NULL);
      if (len<1) break;

      onMessage(1,(const unsigned char *)buf,len);
    }
  }

  virtual void run_output(WDL_FastString &results)
  {
    static char hdr[16] = { '#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0, 0, 0, 1, 0, 0, 0 };

    // send m_sendq as UDP blocks
    if (m_sendq.Available()<=16)
    {
      if (m_sendq.Available()>0) m_sendq.Clear();
      return;
    }
    // m_sendq should begin with a 16 byte pad, then messages in OSC

    char* packetstart=(char*)m_sendq.Get();
    int packetlen=16;
    bool hasbundle=false;
    m_sendq.Advance(16); // skip bundle for now, but keep it around

    SET_SOCK_BLOCK(m_sendsock, true);

    while (m_sendq.Available() >= (int)sizeof(int))
    {
      int len=*(int*)m_sendq.Get(); // not advancing
      OSC_MAKEINTMEM4BE((char*)&len);

      if (len < 1 || len > MAX_OSC_MSG_LEN || len > m_sendq.Available()) break;             
        
      if (packetlen > 16 && packetlen+(int)sizeof(int)+len > m_maxpacketsz)
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

        sendto(m_sendsock, packetstart, packetlen, 0, (struct sockaddr*)&m_sendaddr, sizeof(m_sendaddr));
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
      sendto(m_sendsock, packetstart, packetlen, 0, (struct sockaddr*)&m_sendaddr, sizeof(m_sendaddr));
      if (m_sendsleep>0) Sleep(m_sendsleep);
    }
    SET_SOCK_BLOCK(m_sendsock, false);

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

  SOCKET m_sendsock;
  int m_maxpacketsz, m_sendsleep;
  struct sockaddr_in m_sendaddr;
  WDL_Queue m_sendq;
  WDL_String m_dest;

  struct sockaddr_in m_recvaddr;
  WDL_Queue m_recvq;

};



void scriptInstance::compileCode(int parsestate, const WDL_FastString &curblock, WDL_FastString &results, int lineoffs)
{
  if (parsestate<0 || (unsigned int)parsestate >= sizeof(m_code)/sizeof(m_code[0])) return;

  if (m_code[parsestate])
  {
    results.AppendFormatted(1024,"\tWarning: duplicate %s sections, ignoring later copy\r\n",g_code_names[parsestate]);
    return;
  }


  m_code[parsestate]=NSEEL_code_compile_ex(m_vm,curblock.Get(),lineoffs,NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS);  

  char *err;
  if (!m_code[parsestate] && (err=NSEEL_code_getcodeerror(m_vm)))
  {
    results.AppendFormatted(1024,"\tError: in %s: %s\r\n",g_code_names[parsestate], err);
  }
}

// destdevice, fmtstr, value, ...
EEL_F NSEEL_CGEN_CALL scriptInstance::_send_oscevent(void *opaque, INT_PTR np, EEL_F **parms)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (_this && np > 1)
  {
    int output_idx = (int) floor(parms[0][0]+0.5);
    ioDevice *output = _this->m_devs.Get(output_idx - DEVICE_INDEX_BASE);
    if (!output && output_idx >= 0)
    {
      _this->DebugOutput("oscsend(): output device %f invalid",parms[0][0]);
    }
    if (output || output_idx == -1 || output_idx==-100)
    {
      const char *fmt = _this->GetStringForIndex(parms[1][0]);
      if (fmt)
      {
        int nv=0;
        const char *fmt_types = fmt;
        while (fmt[0] && fmt[0] != '/') 
        {
          if (*fmt != 't') nv++;
          fmt++;
        }
        if (fmt == fmt_types)
        {
          if (np == 2) nv=0;  // if no type specified and no parameters, toggle!
          else nv=1; // otherwise send one parameter
        }

        char buf[1024+128];
        if (eel_format_strings(opaque,fmt,NULL,buf,(int)sizeof(buf), wdl_max((int)np-2-nv,0), parms+2+nv))
        {
          OscMessageWrite wr;
          wr.PushWord(buf);
        
          int x;
          for (x = 0; x < nv;  x++)
          {
            char fmt_type = fmt_types[x];
            double val = x+2 < np ? parms[x+2][0] : 0.0;

            if (fmt_type == 'b') wr.PushIntArg(!!(int) val);
            else if (fmt_type =='i') wr.PushIntArg((int) val);
            else if (fmt_type == 's')
            {
              const char *strval = _this->GetStringForIndex(val);
              if (strval)
              {
                wr.PushStringArg(strval);
              }
              else
              {
                char tmp[64];
                sprintf(tmp,"%.2f",val);
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
              wr.PushFloatArg((float)val);
            }
          }
  
          int l=0;
          const char *ret=wr.GetBuffer(&l);
          if (ret && l>0) 
          {
            if (g_input_omni_outs[1]) 
            {
              g_input_omni_outs[1]->onMessage(1,(unsigned char *)ret,l);
            }

            if (output)
            {
              output->oscSend(ret,l);
            }
            else if (output_idx==-100)
            {
              for (int n=0;n<g_devices.GetSize();n++)
                g_devices.Get(n)->oscSend(ret,l);
            }
            else 
            {
              int n;
              for (n=0;n<_this->m_devs.GetSize();n++)
                _this->m_devs.Get(n)->oscSend(ret,l);
            }
          }
          return 1.0;
        }
        else
        {
          _this->DebugOutput("oscsend(): bad format string '%s'",fmt);
        }
      }
      else
      {
        _this->DebugOutput("oscsend(): bad format index %f",parms[1][0]);
      }
    }
  }
  return 0.0;
}


EEL_F NSEEL_CGEN_CALL scriptInstance::_osc_parm(void *opaque, INT_PTR np, EEL_F **parms)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (np > 1) parms[1][0] = 0.0;
  if (_this && _this->m_cur_oscmsg && np > 0)
  {
    const int idx = (int) parms[0][0];

    char c=0;
    const void *ptr=_this->m_cur_oscmsg->GetIndexedArg(idx&65535,&c);
    if (!ptr) return 0.0;

    if (np > 1) parms[1][0] = (EEL_F)c;

    if (np > 2)
    {
      WDL_FastString *wr=NULL;
      _this->GetStringForIndex(parms[2][0],&wr,true);
      if (wr)
      {
        if (c=='s') 
        {
          wr->Set((const char *)ptr);
          return 1.0;
        }
        wr->Set("");
      }
    }

    if (c=='f') return (EEL_F) *(const float *)ptr;
    if (c=='i') return (EEL_F) *(const int *)ptr;
    if (c=='s') 
    {
      const char *s=(const char *)ptr;
      int idx2=(idx>>16)&1023;
      if (idx2==0 && !*s) return 0.001; // return nonzero if an empty string and requesting first character

      while (idx2>0 && *s) { s++; idx2--; }
      return (EEL_F)*s;
    }
  }
  return 0.0;
}



EEL_F NSEEL_CGEN_CALL scriptInstance::_osc_match(void *opaque, INT_PTR np, EEL_F **parms)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (_this && _this->m_cur_oscmsg && np >= 1)
  {
    WDL_FastString *fmt_wr=NULL;
    const char *fmt = _this->GetStringForIndex(parms[0][0],&fmt_wr);
    if (fmt)
    {
      const char *msg = _this->m_cur_oscmsg->GetMessage();

      if (msg) return eel_string_match(opaque,fmt,msg,0,true,
          fmt + (fmt_wr ? fmt_wr->GetLength() : strlen(fmt)),msg+strlen(msg), 
          (int)np-1, parms+1)
        ? 1.0 : 0.0;
    }
  }
  return 0.0;
}

EEL_F NSEEL_CGEN_CALL scriptInstance::_get_device_open_time(void *opaque, EEL_F *device)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (_this)
  {
    int output_idx = (int) floor(*device+0.5);
    ioDevice *output = _this->m_devs.Get(output_idx - DEVICE_INDEX_BASE);
    if (output) return output->m_last_open_time;
  }
  return 0.0;
}

EEL_F NSEEL_CGEN_CALL scriptInstance::_send_midievent(void *opaque, EEL_F *dest_device)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (_this)
  {
    int output_idx = (int) floor(*dest_device+0.5);
    ioDevice *output = _this->m_devs.Get(output_idx - DEVICE_INDEX_BASE);
    if (!output && output_idx>=0)
    {
      _this->DebugOutput("midisend(): device %f invalid",*dest_device);
    }
    if (output || output_idx == -1 || output_idx==-100)
    {
      unsigned char msg[3];
      EEL_F **vms = _this->m_var_msgs;
      msg[0] = vms[0] ? (int) (vms[0][0]) : 0;
      msg[1] = vms[1] ? (int) (vms[1][0]) : 0;
      msg[2] = vms[2] ? (int) (vms[2][0]) : 0;

      if (g_input_omni_outs[0]) g_input_omni_outs[0]->onMessage(0,msg,3);

      if (output)
      {
        output->midiSend(msg,3);
      }
      else if (output_idx==-100)
      {
        for (int n=0;n<g_devices.GetSize();n++)
          g_devices.Get(n)->midiSend(msg,3);
      }
      else 
      {
        int n;
        for (n=0;n<_this->m_devs.GetSize();n++)
          _this->m_devs.Get(n)->midiSend(msg,3);
      }
      return 1.0;
    }
  }
  return 0.0;
}

EEL_F NSEEL_CGEN_CALL scriptInstance::_send_midievent_str(void *opaque, EEL_F *dest_device, EEL_F *strptr)
{
  scriptInstance *_this = (scriptInstance*)opaque;
  if (_this)
  {
    int output_idx = (int) floor(*dest_device+0.5);
    ioDevice *output = _this->m_devs.Get(output_idx - DEVICE_INDEX_BASE);
    if (!output && output_idx>=0)
    {
      _this->DebugOutput("midisend_str(): device %f invalid",*dest_device);
    }

    WDL_FastString *fs=NULL;
    const char *fmt = _this->GetStringForIndex(*strptr, &fs);
    const int msglen = fs ? fs->GetLength() : fmt ? (int)strlen(fmt) : 0;
    if (fmt && msglen > 0 && (output || output_idx == -1 || output_idx==-100))
    {
      const unsigned char *msg = (const unsigned char *)fmt;
      if (g_input_omni_outs[0]) g_input_omni_outs[0]->onMessage(0,msg,msglen);

      if (output)
      {
        output->midiSend(msg,msglen);
      }
      else if (output_idx==-100)
      {
        for (int n=0;n<g_devices.GetSize();n++)
          g_devices.Get(n)->midiSend(msg,msglen);
      }
      else 
      {
        int n;
        for (n=0;n<_this->m_devs.GetSize();n++)
          _this->m_devs.Get(n)->midiSend(msg,msglen);
      }
      return 1.0;
    }
  }
  return 0.0;
}


void scriptInstance::init_vm()
{
  m_vm = NSEEL_VM_alloc();
  NSEEL_VM_SetCustomFuncThis(m_vm,this);
  eel_string_initvm(m_vm);
  m_lice_state = new eel_lice_state(m_vm,this,1024,64);
  m_net_state = new eel_net_state(4096,NULL);


  m_var_time = NSEEL_VM_regvar(m_vm,"time");
  m_var_msgs[0] = NSEEL_VM_regvar(m_vm,"msg1");
  m_var_msgs[1] = NSEEL_VM_regvar(m_vm,"msg2");
  m_var_msgs[2] = NSEEL_VM_regvar(m_vm,"msg3");
  m_var_msgs[3] = NSEEL_VM_regvar(m_vm,"msgdev");
  m_var_msgs[4] = NSEEL_VM_regvar(m_vm,"oscstr");
  if (m_var_msgs[4]) m_var_msgs[4][0] = -1.0;

  int x;
  for (x=0;x < MAX_OSC_FMTS;x++)
  {
    char tmp[32];
    sprintf(tmp,"fmt%d",x);
    m_var_fmt[x] = NSEEL_VM_regvar(m_vm,tmp);
  }
}

void scriptInstance::import_script(const char *fn, const char *callerfn, WDL_FastString &results)
{
  FILE *fp=NULL;
  WDL_FastString usefn;
  // resolve path relative to current
  int x;
  for (x=0;x<2; x ++)
  {
#ifdef _WIN32
    if (!x && ((fn[0] == '\\' && fn[1] == '\\') || (fn[0] && fn[1] == ':')))
#else
    if (!x && fn[0] == '/')
#endif
    {
      usefn.Set(fn);
    }
    else
    {
      const char *fnu = fn;
      if (x)
      {
        while (*fnu) fnu++;
        while (fnu >= fn && *fnu != '\\' && *fnu != '/') fnu--;
        if (fnu < fn) break;
        fnu++;
      }

      usefn.Set(callerfn);
      int l=usefn.GetLength();
      while (l > 0 && usefn.Get()[l-1] != '\\' && usefn.Get()[l-1] != '/') l--;
      if (l > 0) 
      {
        usefn.SetLen(l);
        usefn.Append(fnu);
      }
      else
      {
        usefn.Set(fnu);
      }
      int last_slash_pos=-1;
      for (l = 0; l < usefn.GetLength(); l ++)
      {
        if (usefn.Get()[l] == '/' || usefn.Get()[l] == '\\')
        {
          if (usefn.Get()[l+1] == '.' && usefn.Get()[l+2] == '.' && 
              (usefn.Get()[l+3] == '/' || usefn.Get()[l+3] == '\\'))
          {
            if (last_slash_pos >= 0)
              usefn.DeleteSub(last_slash_pos, l+3-last_slash_pos);
            else
              usefn.DeleteSub(0,l+3+1);
          }
          else
          {
            last_slash_pos=l;
          }
        }
      // take currentfn, remove filename part, add fnu
      }
    }

    fp = fopen(usefn.Get(),"r");
    if (fp) 
    {
      if (m_loaded_fnlist.Get(usefn.Get())) 
      {
        fclose(fp);
        return; // already imported
      }
      m_loaded_fnlist.Insert(usefn.Get(),true);
      fn = usefn.Get();
      break;
    }
  }
  if (!fp)
  {
    results.AppendFormatted(512,"\tWarning: @import could not open '%s' (%s)\r\n",fn,usefn.Get());
    return;
  }

  bool comment_state=false;
  WDL_FastString curblock;
  for (;;)
  {
    char linebuf[8192];
    linebuf[0]=0;
    fgets(linebuf,sizeof(linebuf),fp);

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
      if (!strcmp(lp.gettoken_str(0),"@import"))
      {
        if (lp.getnumtokens()>1)
          import_script(lp.gettoken_str(1),fn,results);
      }
      curblock.Append(";");
    }
    else curblock.Append(linebuf);
    curblock.Append("\n");
  }
  fclose(fp);

  NSEEL_CODEHANDLE ch=NSEEL_code_compile_ex(m_vm,curblock.Get(),0,NSEEL_CODE_COMPILE_FLAG_COMMONFUNCS);  
  if (ch)
  {
    m_imported_code.Add(ch);
  }
  else
  {
    const char *err=NSEEL_code_getcodeerror(m_vm);
    if (err)
      results.AppendFormatted(512,"\tWarning:%s:%s\r\n",fn,err);
  }
}

void scriptInstance::load_script(WDL_FastString &results)
{
  results.Append(m_fn.Get());
  results.Append("\r\n");

  FILE *fp = fopen(m_fn.Get(),"r");
  if (!fp)
  {
    results.Append("\tError: failed opening script.");
    return;
  }
  m_loaded_fnlist.Insert(m_fn.Get(),true);

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
      for (x=0;x<(int)(sizeof(g_code_names)/sizeof(g_code_names[0])) && strcmp(tok,g_code_names[x]);x++);

      if (x < (int)(sizeof(g_code_names)/sizeof(g_code_names[0])))
      {
        compileCode(parsestate,curblock,results,cursec_lineoffs);
        parsestate=x;
        cursec_lineoffs=lineoffs;
        curblock.Set("");
      }
      else if (!strcmp(tok,"@import"))
      {
        if (lp.getnumtokens()!=2)
        {
          results.Append("\tUsage: @import filename\r\n");
        }
        else
        {
          import_script(lp.gettoken_str(1),m_fn.Get(),results);
        }
      }
      else if (!strcmp(tok,"@input"))
      {
        int this_type;

        if (lp.getnumtokens()<3 || !lp.gettoken_str(1)[0] || (this_type = lp.gettoken_enum(2,"MIDI\0OSC\0OMNI-MIDI\0OMNI-OSC\0OMNI-MIDI-OUTPUT\0OMNI-OSC-OUTPUT\0"))<0 ||
          (this_type < 2 && lp.getnumtokens()<4))
        {
          results.Append("\tUsage: @input devicehandle MIDI \"substring devicename match\" [skip_count]\r\n");
          results.Append("\tUsage: @input devicehandle OSC \"1.2.3.4:port\"\r\n");
          results.Append("\tUsage: @input devicehandle OSC \"*:port\"\r\n");
          results.Append("\tUsage: @input devicehandle OMNI-MIDI\r\n");
          results.Append("\tUsage: @input devicehandle OMNI-OSC\r\n");
          results.Append("\tUsage: @input devicehandle OMNI-MIDI-OUTPUT\r\n");
          results.Append("\tUsage: @input devicehandle OMNI-OSC-OUTPUT\r\n");
        }
        else
        {
          if (NSEEL_VM_get_var_refcnt(m_vm,lp.gettoken_str(1))>=0)
          {
            results.AppendFormatted(1024,"\tWarning: device name '%s' already in use, skipping @input line\r\n",lp.gettoken_str(1));
          }
          else
          {            
            if (this_type==3||this_type==2) // OMNI-OSC or OMNI-MIDI
            {
              omniInputDevice *r = new omniInputDevice(this_type == 3 ? "OSC" : "MIDI");
              EEL_F *dev_idx = NSEEL_VM_regvar(m_vm,lp.gettoken_str(1));
              if (dev_idx) dev_idx[0] = m_devs.GetSize() + DEVICE_INDEX_BASE;
              r->addinst(messageCallback,this,dev_idx);
              m_devs.Add(r);
              
              g_omni_inputs_fordelete.Add(r);
              // do NOT add OMNI instances to g_devices! :)
            }
            else if (this_type==5||this_type==4) // OMNI-OSC or OMNI-MIDI
            {
              const int w=this_type==5;
              if (!g_input_omni_outs[w]) g_input_omni_outs[w]=new omniInputDevice("");

              omniInputDevice *r = g_input_omni_outs[w];
              EEL_F *dev_idx = NSEEL_VM_regvar(m_vm,lp.gettoken_str(1));
              if (dev_idx) dev_idx[0] = m_devs.GetSize() + DEVICE_INDEX_BASE;
              r->addinst(messageCallback,this,dev_idx);
              m_devs.Add(r);
              
              // do NOT add OMNI-OUT instances to g_devices either! :)
            }
            else if (this_type==1) // OSC
            {
              char buf[512];
              const char *dp=lp.gettoken_str(3);
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

              bool is_reuse=false;
              oscDevice *r=NULL;
              for (x=0; x < g_devices.GetSize(); x++)
              {
                ioDevice *dev = g_devices.Get(x);
                if (dev && !strcmp(dev->get_type(),"OSC") && dev->m_has_input)
                {
                  oscDevice *od = (oscDevice *)dev;
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
                r = new oscDevice(NULL,0,-1,&addr);
                if (r->m_sendsock == INVALID_SOCKET)
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
                if (dev_idx) dev_idx[0] = m_devs.GetSize() + DEVICE_INDEX_BASE;
                r->addinst(messageCallback,this,dev_idx);
                m_devs.Add(r);

                if (!is_reuse) g_devices.Add(r);
              }


            }
            else if (this_type==0) // MIDI
            {
              const char *substr = lp.gettoken_str(3);
              int skipcnt = lp.getnumtokens()>=4 ? lp.gettoken_int(4) : 0;

              midiInputDevice *rec = new midiInputDevice(substr,skipcnt, &g_devices);
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
              if (dev_idx) dev_idx[0] = m_devs.GetSize() + DEVICE_INDEX_BASE;
              rec->addinst(messageCallback,this,dev_idx);
              m_devs.Add(rec);

              if (!is_reuse) g_devices.Add(rec);
            }
          }
        }
      }
      else if (!strcmp(tok,"@output"))
      {
        int this_type;
        if (lp.getnumtokens()<4 || !lp.gettoken_str(1)[0]||(this_type = lp.gettoken_enum(2,"MIDI\0OSC\0"))<0)
        {
          results.Append("\tUsage: @output devicehandle OSC \"host:port\" [maxpacketsize (def=1024)] [sleepinMS (def=10)]\r\n");
          results.Append("\tUsage: @output devicehandle MIDI \"substring match\" [skip]\r\n");
        }
        else
        {
          if (NSEEL_VM_get_var_refcnt(m_vm,lp.gettoken_str(1))>=0)
          {
            results.AppendFormatted(1024,"\tWarning: device name '%s' already in use, skipping @output line\r\n",lp.gettoken_str(1));
          }
          else
          {
            if (this_type == 0)
            {
              const char *substr = lp.gettoken_str(3);
              int skipcnt = lp.getnumtokens()>=4 ? lp.gettoken_int(4) : 0;

              bool is_reuse=false;
              midiOutputDevice *rec = new midiOutputDevice(substr,skipcnt, &g_devices);
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
              if (var) *var=m_devs.GetSize() + DEVICE_INDEX_BASE;
              m_devs.Add(rec);

              if (!is_reuse) g_devices.Add(rec);


            }
            else if (this_type == 1)
            {
              const char *dp = lp.gettoken_str(3);
              oscDevice *r = NULL;
              bool is_reuse=false;
              for (x=0;x<g_devices.GetSize();x++)
              {
                ioDevice *d = g_devices.Get(x);
                if (d && !strcmp(d->get_type(),"OSC") && d->m_has_output)
                {
                  oscDevice *p = (oscDevice *)d;
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
                r = new oscDevice(dp, lp.getnumtokens()>4 ? lp.gettoken_int(4) : 0,
                                      lp.getnumtokens()>5 ? lp.gettoken_int(5) : -1, NULL);
                if (r->m_sendsock == INVALID_SOCKET)
                {
                  results.AppendFormatted(1024,"\tWarning: failed creating destination for @output '%s' OSC '%s'\r\n",lp.gettoken_str(1),lp.gettoken_str(3));
                  delete r;
                  r=NULL;
                }
              }

              if (r)
              {
                EEL_F *dev_idx = NSEEL_VM_regvar(m_vm,lp.gettoken_str(1));
                if (dev_idx) dev_idx[0] = m_devs.GetSize() + DEVICE_INDEX_BASE;
                r->addinst(messageCallback,this,dev_idx);
                m_devs.Add(r);

                if (!is_reuse) g_devices.Add(r);
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
          p++;
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

  int n_in=0, n_out=0, n_bidir=0;
  for (int x=0;x<m_devs.GetSize();x++)
  {
    ioDevice *dev = m_devs.Get(x);
    if (dev->m_has_input && dev->m_has_output) n_bidir++;
    else if (dev->m_has_input) n_in++;
    else if (dev->m_has_output) n_out++;
  }
  results.AppendFormatted(512,"\t%d inputs, %d outputs, %d bidirectional\r\n\r\n",
      n_in,n_out,n_bidir);
}

void scriptInstance::messageCallback(void *d1, void *d2, char type, int len, void *msg)
{
  scriptInstance *_this  = (scriptInstance *)d1;
  if (_this && msg)
  {
    // MIDI
    if (_this->m_incoming_events.GetSize() < 65536*8)
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

  int needGfxRef=0;
  if (m_lice_state && m_lice_state->hwnd_standalone)
  {
    RECT r;
    GetClientRect(m_lice_state->hwnd_standalone,&r);
    needGfxRef=m_lice_state->setup_frame(m_lice_state->hwnd_standalone,r);
  }

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
    while (pos < endpos+1 - (int)sizeof(incomingEvent))
    {
      incomingEvent *evt = (incomingEvent*) ((char *)tmp.Get()+pos);
      
      const int this_sz = ((sizeof(incomingEvent) + (evt->sz-3)) + 7) & ~7;

      if (pos+this_sz > endpos) break;
      pos += this_sz;

      switch (evt->type)
      {
        case 0:
          if (evt->sz > 3)
          {
            if (m_var_msgs[0]) m_var_msgs[0][0] = 0;
            if (m_var_msgs[1]) m_var_msgs[1][0] = 0;
            if (m_var_msgs[2]) m_var_msgs[2][0] = 0;
            if (m_var_msgs[3]) m_var_msgs[3][0] = evt->dev_ptr ? *evt->dev_ptr : -1.0;
            if (m_var_msgs[4]) m_var_msgs[4][0] = MIDI_CURMSG_STRING;
            m_cur_midi_sysex_msg.SetRaw((char*)evt->msg,evt->sz);
            NSEEL_code_execute(m_code[2]);
            m_cur_midi_sysex_msg.Set("");
            if (m_var_msgs[4]) m_var_msgs[4][0] = -1;
          }
          else if (evt->sz == 3)
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
            if (m_var_msgs[4]) m_var_msgs[4][0] = -1;
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
            if (m_var_msgs[3]) m_var_msgs[3][0] = evt->dev_ptr ? *evt->dev_ptr : -1.0;
            while (rd_pos + rd_sz <= evt->sz && rd_sz>=0)
            {
              OscMessageRead rmsg((char*)evt->msg + rd_pos, rd_sz);

              const char *mstr = rmsg.GetMessage();
              if (mstr && *mstr)
              {
                if (m_var_msgs[4]) m_var_msgs[4][0] = OSC_CURMSG_STRING;
                m_cur_oscmsg = &rmsg;
                NSEEL_code_execute(m_code[3]);
                m_cur_oscmsg = NULL;
                if (m_var_msgs[4]) m_var_msgs[4][0] = -1;
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

  if (m_lice_state->hwnd_standalone && (needGfxRef || (m_lice_state && m_lice_state->m_framebuffer_dirty)))
  {
    InvalidateRect(m_lice_state->hwnd_standalone,NULL,FALSE);
  }

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
  g_omni_inputs_fordelete.Empty(true);
  delete g_input_omni_outs[0];
  g_input_omni_outs[0]=NULL;
  delete g_input_omni_outs[1];
  g_input_omni_outs[1]=NULL;
  g_devices.Empty(true);
  g_scripts.Empty(true);

  int x;
  for (x=0;x<g_script_load_paths.GetSize();x++)
  {
    results.AppendFormatted(512,"===== Loading scripts from %s:\r\n\r\n",g_script_load_paths.Get(x));
    load_scripts_for_path(g_script_load_paths.Get(x),results);
  }
  for (x=0;x<g_script_load_filenames.GetSize();x++)
    g_scripts.Add(new scriptInstance(g_script_load_filenames.Get(x),results));

  int n_in=0, n_out=0, n_bidir=0;
  for (x=0;x<g_devices.GetSize();x++)
  {
    ioDevice *dev = g_devices.Get(x);
    if (dev->m_has_input && dev->m_has_output) n_bidir++;
    else if (dev->m_has_input) n_in++;
    else if (dev->m_has_output) n_out++;
  }

  results.AppendFormatted(512,"Total: %d scripts, %d inputs %d outputs %d bidirectional\r\n", g_scripts.GetSize(), n_in, n_out,n_bidir);

  results.Append("\r\n");
  for (x=0;x<80;x++) results.Append("=");
  results.Append("\r\n");

  // propagate any input instances to omni
  for (x=0;x<g_devices.GetSize();x++)
  {
    ioDevice *dev = g_devices.Get(x);
    const char *dev_type = dev->get_type();
    if (!dev->m_has_input) continue;

    int i;
    for (i=0;i<g_scripts.GetSize();i++)
    {
      scriptInstance *scr = g_scripts.Get(i);
      if (scr->m_devs.Find(dev)>=0) continue;

      int a;
      for (a=0;a<scr->m_devs.GetSize();a++)
      {
        ioDevice *thisdev = scr->m_devs.Get(a);
        if (!strcmp(thisdev->get_type(),"OMNI"))
        {
          omniInputDevice *omni = (omniInputDevice *)thisdev;
          if (!strcmp(omni->getOmniType(),dev_type))
          {
            omni->copyRecsTo(dev);
            break;
          }
        }
      }
    }
  }


  for (x=0;x<g_scripts.GetSize(); x++) g_scripts.Get(x)->start(results);

  for (x=0;x<g_devices.GetSize();x++)
  {
    ioDevice *rec=g_devices.Get(x);
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
      {
        const char *vs="Cockos OSCII-bot v" OSCIIBOT_VERSION ;
#ifdef _WIN32
        HICON icon=LoadIcon(g_hInstance,MAKEINTRESOURCE(IDI_ICON1));
        SetClassLongPtr(hwndDlg,GCLP_HICON,(LPARAM)icon);
        systray_add(hwndDlg, 0, (HICON)icon, (char*)vs);
#endif
        SetWindowText(hwndDlg,vs);
      }
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
        HFONT font = CreateFont(12, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Courier New");
#endif
        if (font) SendDlgItemMessage(hwndDlg,IDC_EDIT1,WM_SETFONT,(WPARAM)font,0);
      }


      if (g_last_wndpos.right > 0 && g_last_wndpos.bottom != 0)
      {
        g_last_wndpos.right += g_last_wndpos.left;
        g_last_wndpos.bottom += g_last_wndpos.top;
        SetWindowPos(hwndDlg,NULL,g_last_wndpos.left,g_last_wndpos.top,g_last_wndpos.right-g_last_wndpos.left,g_last_wndpos.bottom-g_last_wndpos.top,SWP_NOZORDER|SWP_NOACTIVATE);
      }
#ifndef _WIN32
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
#ifdef __APPLE__
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
        for (x=0;x<g_devices.GetSize();x++) 
          g_devices.Get(x)->run_input(results);

        bool needUIupdate=false;

        double curtime = timeGetTime()/1000.0;
        for (x=0;x<g_scripts.GetSize();x++)
        {
          if (g_scripts.Get(x)->run(curtime,results)) needUIupdate=true;
        }

        for (x=0;x<g_devices.GetSize();x++) 
          g_devices.Get(x)->run_output(results);  // send queued messages

        if (results.GetLength() != asz||g_force_results_update)
        {
          g_force_results_update=false;
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
          for(x=0;x<(int)(sizeof(g_recent_events)/sizeof(g_recent_events[0]));x++)
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
  NSEEL_addfunc_retval("get_device_open_time",1,NSEEL_PProc_THIS,&scriptInstance::_get_device_open_time);
  NSEEL_addfunc_retval("midisend",1,NSEEL_PProc_THIS,&scriptInstance::_send_midievent);
  NSEEL_addfunc_retval("midisend_str",2,NSEEL_PProc_THIS,&scriptInstance::_send_midievent_str);
  NSEEL_addfunc_varparm("oscsend",2,NSEEL_PProc_THIS,&scriptInstance::_send_oscevent);
  NSEEL_addfunc_varparm("oscmatch",1,NSEEL_PProc_THIS,&scriptInstance::_osc_match);
  NSEEL_addfunc_varparm("oscparm",1,NSEEL_PProc_THIS,&scriptInstance::_osc_parm);

  EEL_string_register();
  EEL_file_register();
  EEL_misc_register();
  EEL_eval_register();
  EEL_tcp_register();

  eel_lice_register();

  HICON icon=NULL;
#ifdef _WIN32
  icon = LoadIcon(g_hInstance,MAKEINTRESOURCE(IDI_ICON1));
#endif
  eel_lice_register_standalone(g_hInstance,"OSCII-bot-gfx", NULL, icon);

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
        if (!strcmp(parm,"-doc"))
        {
          doc_Generate();
        }
        else if (!strcmp(parm,"-dir"))
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

  char tmp[2048];
  tmp[0]=0;
  GetModuleFileName(NULL,tmp,sizeof(tmp));
  WDL_remove_filepart(tmp);
  g_default_script_path.Set(tmp);

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

  g_devices.Empty(true);
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
#ifdef __APPLE__
extern "C" {
#endif

const char **g_argv;
int g_argc;

#ifdef __APPLE__
};
#endif

INT_PTR SWELLAppMain(int msg, INT_PTR parm1, INT_PTR parm2)
{
  switch (msg)
  {
    case SWELLAPP_ONLOAD:
      {
        char tmp[2048];
        tmp[0]=0;
        GetModuleFileName(NULL,tmp,sizeof(tmp));
        WDL_remove_filepart(tmp);
        g_default_script_path.Set(tmp);
        
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
            g_default_script_path.Append(
#ifdef __APPLE__
                "/Library/Application Support/OSCII-bot"
#else
                "/.config/OSCII-bot"
#endif
                );
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
            "If no script files specified, default will be all txt files in"
#ifdef __APPLE__
                "~/Library/Application Support/OSCII-bot"
#else
                "~/.config/OSCII-bot"
#endif
                ,g_argv[0]);
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
      g_devices.Empty(true);
      g_scripts.Empty(true);
    break;
    case SWELLAPP_PROCESSMESSAGE:
      if (parm1)
      {
        MSG *m = (MSG *)parm1;
        if (m->hwnd && (m->message == WM_KEYDOWN || m->message == WM_KEYUP || m->message == WM_CHAR))
        {
          int x;
          for(x=0;x<g_scripts.GetSize();x++)
          {
            scriptInstance *scr = g_scripts.Get(x);
            if (scr->m_lice_state && scr->m_lice_state->hwnd_standalone == m->hwnd)
            {
              SendMessage(m->hwnd,m->message,m->wParam,m->lParam);
              return 1;
            }
          }

        }
      }
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
