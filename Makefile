
NAME=libdatachannel
CXX=$(CROSS)g++
AR=$(CROSS)ar
RM=rm -f
CPPFLAGS=-pthread -std=c++17 -fPIC -Wall -Wno-reorder -Wno-sign-compare -Wno-unused-function -Og -g
LDFLAGS=-pthread
LDLIBS=$(shell pkg-config --libs glib-2.0 gobject-2.0 nice) -lgnutls
INCLUDES=$(shell pkg-config --cflags glib-2.0 gobject-2.0 nice) -I$(USRSCTP_DIR)/usrsctplib

USRSCTP_DIR:=usrsctp
USRSCTP_DEFINES:=-DINET -DINET6
USRSCTP_CFLAGS:=-fPIC -Wno-address-of-packed-member

SRCS=$(shell printf "%s " src/*.cpp)
OBJS=$(subst .cpp,.o,$(SRCS))

all: $(NAME).a $(NAME).so tests

src/%.o: src/%.cpp
	$(CXX) $(CPPFLAGS) $(INCLUDES) $(USRSCTP_DEFINES) -MMD -MP -o $@ -c $<

test/%.o: test/%.cpp
	$(CXX) $(CPPFLAGS) -Isrc -MMD -MP -o $@ -c $<

-include $(subst .o,.d,$(OBJS))

$(NAME).a: $(OBJS)
	$(AR) crf $@ $(OBJS)

$(NAME).so: libusrsctp.a $(OBJS)
	$(CXX) $(LDFLAGS) -shared -o $@ $(OBJS) $(LDLIBS) libusrsctp.a

tests: $(NAME).a test/main.o
	$(CXX) $(LDFLAGS) -o $@ test/main.o $(LDLIBS) $(NAME).a libusrsctp.a

clean:
	$(RM) src/*.o src/*.d

dist-clean: clean
	$(RM) $(NAME).a
	$(RM) $(NAME).so
	$(RM) libusrsctp.a
	$(RM) src/*~

libusrsctp.a:
	cd $(USRSCTP_DIR) && ./bootstrap && CFLAGS="$(USRSCTP_CFLAGS)" ./configure --enable-static && make
	cp $(USRSCTP_DIR)/usrsctplib/.libs/libusrsctp.a .

