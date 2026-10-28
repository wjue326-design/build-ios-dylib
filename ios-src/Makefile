# Makefile — libXSPlugin.dylib（单文件：内核 + 内嵌 HTML 面板）
IOS_SDK    := $(shell xcrun --sdk iphoneos --show-sdk-path)
ARCH       ?= arm64
MIN_IOS    ?= 14.0
USE_DOBBY  ?= 1

DOBBY_INC  := ../third_party/Dobby/include
DOBBY_LIB  := libs/libdobby.$(ARCH).a
DOBBY_LIB_FALLBACK := libs/libdobby.a

CXX        := xcrun --sdk iphoneos clang++
CXXFLAGS   := -arch $(ARCH) -isysroot $(IOS_SDK) -miphoneos-version-min=$(MIN_IOS) \
              -fobjc-arc -std=c++17 -O2 -fvisibility=hidden \
              -I. -Iinclude -DUSE_DOBBY=$(USE_DOBBY)
LDFLAGS    := -dynamiclib -arch $(ARCH) -isysroot $(IOS_SDK) -miphoneos-version-min=$(MIN_IOS) \
              -framework Foundation -framework UIKit -framework WebKit \
              -Wl,-undefined,dynamic_lookup \
              -Wl,-install_name,@rpath/libXSPlugin.dylib
ifeq ($(USE_DOBBY),1)
CXXFLAGS   += -I$(DOBBY_INC)
LIBS       := $(if $(wildcard $(DOBBY_LIB)),$(DOBBY_LIB),$(DOBBY_LIB_FALLBACK))
endif
SRC        := XSPlugin.mm XSPanel.mm XSCrash.mm DnfScan.cpp
OUT        ?= libXSPlugin.dylib
PANEL_HTML := ../script/index.html

all: XSHTML.h $(OUT)

XSHTML.h: $(PANEL_HTML)
	@if command -v xxd >/dev/null 2>&1; then \
	  xxd -i -n XSHTML_DATA $(PANEL_HTML) > $@; \
	else \
	  python3 -c "d=open('$(PANEL_HTML)','rb').read();print('unsigned char XSHTML_DATA[] = {'+','.join(str(b) for b in d)+'};');print('unsigned int XSHTML_DATA_len = %d;'%len(d))" > $@; \
	fi

$(OUT): $(SRC) XSHTML.h
	$(CXX) $(CXXFLAGS) $(SRC) $(LIBS) $(LDFLAGS) -o $@
	@echo "built: $(OUT) ($(ARCH))"

clean:
	rm -f libXSPlugin*.dylib XSHTML.h

.PHONY: all clean
