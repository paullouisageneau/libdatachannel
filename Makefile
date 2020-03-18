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
JUICE_DIR=deps/libjuice
PLOG_DIR=deps/plog

INCLUDES=-Iinclude/rtc -I$(PLOG_DIR)/include -I$(USRSCTP_DIR)/usrsctplib
LDLIBS=

USE_GNUTLS ?= 0
ifneq ($(USE_GNUTLS), 0)
        CPPFLAGS+=-DUSE_GNUTLS=1
        LIBS+=gnutls
else
        CPPFLAGS+=-DUSE_GNUTLS=0
        LIBS+=openssl
endif

USE_JUICE ?= 0
ifneq ($(USE_JUICE), 0)
        CPPFLAGS+=-DUSE_JUICE=1
        INCLUDES+=-I$(JUICE_DIR)/include
        LOCALLIBS+=libjuice.a
ifneq ($(USE_GNUTLS), 0)
        LIBS+=nettle
endif
else
        CPPFLAGS+=-DUSE_JUICE=0
        LIBS+=glib-2.0 gobject-2.0 nice
endif

RTC_ENABLE_WEBSOCKET ?= 1
ifneq ($(RTC_ENABLE_WEBSOCKET), 0)
        CPPFLAGS+=-DRTC_ENABLE_WEBSOCKET=1
else
        CPPFLAGS+=-DRTC_ENABLE_WEBSOCKET=0
endif


INCLUDES+=$(shell pkg-config --cflags $(LIBS))
LDLIBS+=$(LOCALLIBS) $(shell pkg-config --libs $(LIBS))

SRCS=$(shell printf "%s " src/*.cpp)
OBJS=$(subst .cpp,.o,$(SRCS))

TEST_SRCS=$(shell printf "%s " test/*.cpp)
TEST_OBJS=$(subst .cpp,.o,$(TEST_SRCS))

all: $(NAME).a $(NAME).so tests

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) -MMD -MP -o $@ -c $<

test/%.o: test/%.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) -Iinclude -Isrc -MMD -MP -o $@ -c $<

-include $(subst .cpp,.d,$(SRCS))

$(NAME).a: $(OBJS)
	$(AR) crf $@ $(OBJS)

$(NAME).so: $(LOCALLIBS) $(OBJS)
	$(CXX) $(LDFLAGS) -shared -o $@ $(OBJS) $(LDLIBS)

tests: $(NAME).a $(TEST_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(TEST_OBJS) $(NAME).a $(LDLIBS)

clean:
	-$(RM) include/rtc/*.d *.d
	-$(RM) src/*.o src/*.d
	-$(RM) test/*.o test/*.d

dist-clean: clean
	-$(RM) $(NAME).a
	-$(RM) $(NAME).so
	-$(RM) libusrsctp.a
	-$(RM) libjuice.a
	-$(RM) tests
	-$(RM) include/*~
	-$(RM) src/*~
	-$(RM) test/*~
	-cd $(USRSCTP_DIR) && make clean
	-cd $(JUICE_DIR) && make clean

libusrsctp.a:
	cd $(USRSCTP_DIR) && \
		./bootstrap && \
		./configure --enable-static --disable-debug CFLAGS="$(CPPFLAGS) -Wno-error=format-truncation" && \
		make
	cp $(USRSCTP_DIR)/usrsctplib/.libs/libusrsctp.a .

libjuice.a:
ifneq ($(USE_GNUTLS), 0)
	cd $(JUICE_DIR) && make USE_NETTLE=1
else
	cd $(JUICE_DIR) && make USE_NETTLE=0
endif
	cp $(JUICE_DIR)/libjuice.a .

