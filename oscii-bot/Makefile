APPNAME=osciibot

WDL_PATH = ../WDL
vpath swell%.cpp $(WDL_PATH)/swell
vpath lice%.cpp $(WDL_PATH)/lice
vpath png%.c $(WDL_PATH)/libpng
vpath %.c $(WDL_PATH) $(WDL_PATH)/zlib $(WDL_PATH)/eel2
vpath %.cpp $(WDL_PATH) $(WDL_PATH)/wingui $(WDL_PATH)/jnetlib
vpath a2x64.php $(WDL_PATH)/eel2

###### Objects and resources (probably too many)
SWELL_OBJS = swell.o swell-ini.o swell-miscdlg-generic.o swell-wnd-generic.o \
             swell-menu-generic.o swell-kb-generic.o swell-dlg-generic.o \
             swell-gdi-generic.o swell-misc-generic.o swell-gdi-lice.o \
             swell-generic-gdk.o swell-appstub-generic.o swell-modstub-generic.o

LICE_OBJS = lice_image.o lice_arc.o lice_line.o lice_png.o lice_text.o \
            lice_textnew.o lice.o lice_colorspace.o

PNG_OBJS = png.o pngerror.o pngget.o pngmem.o pngpread.o pngread.o \
           pngrio.o pngrtran.o pngrutil.o pngset.o pngtrans.o pngwio.o \
           pngwrite.o pngwtran.o pngwutil.o

ZLIB_OBJS = adler32.o compress.o crc32.o deflate.o infback.o inffast.o inflate.o \
            inftrees.o trees.o uncompr.o zutil.o

JNET_OBJS = asyncdns.o connection.o listen.o util.o

EEL_OBJS = asm-nseel-x64.o nseel-caltab.o nseel-cfunc.o nseel-compiler.o \
           nseel-eval.o nseel-lextab.o nseel-ram.o nseel-yylex.o

OTHER_OBJS = wndsize.o filebrowse.o

RESFILES = res.rc_mac_dlg res.rc_mac_menu

OBJS += oscmsg.o midi_linux.o midi2osc.o main.o $(SWELL_OBJS) $(PNG_OBJS) \
        $(ZLIB_OBJS) $(JNET_OBJS) $(EEL_OBJS) $(LICE_OBJS) $(OTHER_OBJS) 

###### Compiler/Linker flags
CFLAGS += -pipe -fvisibility=hidden -fno-math-errno -fPIC -DPIC -Wall -Wshadow \
          -Wno-unused-function -Wno-multichar -Wno-unused-result

ifndef ALLOW_WARNINGS
  CFLAGS += -Werror
endif
ifndef DEPRECATED_WARNINGS
  CFLAGS +=  -Wno-deprecated-declarations
endif

ifdef DEBUG
  CFLAGS += -O0 -g -D_DEBUG
else
  CFLAGS += -O2 -DNDEBUG
endif
CFLAGS += -DSWELL_LICE_GDI -DPNG_WRITE_SUPPORTED
LFLAGS = -lpthread -ldl -lX11 -lXi -lXfixes

###### GDK
CFLAGS += -DSWELL_TARGET_GDK=3 $(shell pkg-config --cflags gdk-3.0)
LFLAGS += $(shell pkg-config --libs gdk-3.0)

###### Freetype
CFLAGS += -DSWELL_FREETYPE $(shell freetype-config --cflags)
LFLAGS += $(shell freetype-config --libs)

##### Jack
CFLAGS += $(shell pkg-config --cflags jack)
LFLAGS += $(shell pkg-config --libs jack)

CXXFLAGS = $(CFLAGS)


default: $(APPNAME)

.PHONY: clean run

$(RESFILES): res.rc
	php $(WDL_PATH)/swell/mac_resgen.php $^
	
asm-nseel-x64.o: a2x64.php asm-nseel-x86-gcc.c
	cd $(WDL_PATH)/eel2 && php a2x64.php elf64 && cp $@ ../../oscii-bot
	
$(APPNAME): $(RESFILES) $(OBJS)
	$(CXX) -o $@ $(CFLAGS) $(OBJS) $(LFLAGS)

run: $(APPNAME)
	./$^

clean:
	-rm $(OBJS) $(APPNAME) $(RESFILES) $(WDL_PATH)/eel2/asm-nseel-x64.o $(WDL_PATH)/eel2/asm-nseel-x64.asm
