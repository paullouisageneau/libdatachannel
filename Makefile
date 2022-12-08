# libdatachannel

NAME=libdatachannel
CXX=$(CROSS)g++
AR=$(CROSS)ar
RM=rm -f
CXXFLAGS=-std=c++17
CPPFLAGS=-O2 -pthread -fPIC -Wall
LDFLAGS=-pthread
LIBS=
LOCALLIBS=libusrsctp.a
USRSCTP_DIR=deps/usrsctp
SRTP_DIR=deps/libsrtp
JUICE_DIR=deps/libjuice
PLOG_DIR=deps/plog

INCLUDES=-Isrc -Iinclude/rtc -Iinclude -I$(PLOG_DIR)/include -I$(USRSCTP_DIR)/usrsctplib
LDLIBS=

USE_GNUTLS ?= 0
ifneq ($(USE_GNUTLS), 0)
        CPPFLAGS+=-DUSE_GNUTLS=1
        LIBS+=gnutls
else
        CPPFLAGS+=-DUSE_GNUTLS=0
        LIBS+=openssl
endif

USE_NICE ?= 0
ifneq ($(USE_NICE), 0)
        CPPFLAGS+=-DUSE_NICE=1
        LIBS+=glib-2.0 gobject-2.0 nice
else
        CPPFLAGS+=-DUSE_NICE=0
        INCLUDES+=-I$(JUICE_DIR)/include
        LOCALLIBS+=libjuice.a
ifneq ($(USE_GNUTLS), 0)
        LIBS+=nettle
endif
endif

NO_MEDIA ?= 0
USE_SYSTEM_SRTP ?= 0
ifeq ($(NO_MEDIA), 0)
        CPPFLAGS+=-DRTC_ENABLE_MEDIA=1
ifneq ($(USE_SYSTEM_SRTP), 0)
        CPPFLAGS+=-DRTC_SYSTEM_SRTP=1
        LIBS+=srtp
else
        CPPFLAGS+=-DRTC_SYSTEM_SRTP=0
		INCLUDES+=-I$(SRTP_DIR)/include
        LOCALLIBS+=libsrtp2.a
endif
else
        CPPFLAGS+=-DRTC_ENABLE_MEDIA=0
endif

NO_WEBSOCKET ?= 0
ifeq ($(NO_WEBSOCKET), 0)
        CPPFLAGS+=-DRTC_ENABLE_WEBSOCKET=1
else
        CPPFLAGS+=-DRTC_ENABLE_WEBSOCKET=0
endif

CPPFLAGS+=-DRTC_EXPORTS

INCLUDES+=$(if $(LIBS),$(shell pkg-config --cflags $(LIBS)),)
LDLIBS+=$(LOCALLIBS) $(if $(LIBS),$(shell pkg-config --libs $(LIBS)),)

SRCS=$(shell printf "%s " src/*.cpp src/impl/*.cpp)
OBJS=$(subst .cpp,.o,$(SRCS))

TEST_SRCS=$(shell printf "%s " test/*.cpp)
TEST_OBJS=$(subst .cpp,.o,$(TEST_SRCS))

all: $(NAME).a $(NAME).so tests

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) -MMD -MP -o $@ -c $<

test/%.o: test/%.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) -Iinclude -Isrc -MMD -MP -o $@ -c $<

-include $(subst .cpp,.d,$(SRCS))

$(NAME).a: $(LOCALLIBS) $(OBJS)
	$(AR) crf $@ $(OBJS)

$(NAME).so: $(LOCALLIBS) $(OBJS)
	$(CXX) $(LDFLAGS) -shared -o $@ $(OBJS) $(LDLIBS)

tests: $(NAME).a $(TEST_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(TEST_OBJS) $(NAME).a $(LDLIBS)

clean:
	-$(RM) include/rtc/*.d *.d
	-$(RM) src/*.o src/*.d
	-$(RM) src/impl/*.o src/impl/*.d
	-$(RM) test/*.o test/*.d

dist-clean: clean
	-$(RM) $(NAME).a
	-$(RM) $(NAME).so
	-$(RM) libusrsctp.a
	-$(RM) libjuice.a
	-$(RM) libsrtp2.a
	-$(RM) tests
	-$(RM) include/*~
	-$(RM) src/*~
	-$(RM) test/*~
	-cd $(USRSCTP_DIR) && make clean
	-cd $(SRTP_DIR) && make clean
	-cd $(JUICE_DIR) && make clean

libusrsctp.a:
	cd $(USRSCTP_DIR) && \
		./bootstrap && \
		./configure --enable-static --disable-programs --disable-debug \
			--disable-inet --disable-inet6 CFLAGS="-fPIC" && \
		make
	cp $(USRSCTP_DIR)/usrsctplib/.libs/libusrsctp.a .

libsrtp2.a:
	cd $(SRTP_DIR) && \
		./configure && \
		make
	cp $(SRTP_DIR)/libsrtp2.a .

libjuice.a:
ifneq ($(USE_GNUTLS), 0)
	cd $(JUICE_DIR) && make USE_NETTLE=1
else
	cd $(JUICE_DIR) && make USE_NETTLE=0
endif
	cp $(JUICE_DIR)/libjuice.a .

