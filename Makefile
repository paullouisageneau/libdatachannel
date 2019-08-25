
NAME=libdatachannel
CXX=$(CROSS)g++
AR=$(CROSS)ar
RM=rm -f
CPPFLAGS=-pthread -std=c++17 -fPIC -Wall -Wno-reorder -Wno-sign-compare -Wno-unused-function -Og -g
LDFLAGS=-pthread
LDLIBS=$(shell pkg-config --libs glib-2.0 gobject-2.0 nice) -lgnutls
INCLUDES=$(shell pkg-config --cflags glib-2.0 gobject-2.0 nice) -I$(USRSCTP_DIR)/usrsctplib

USRSCTP_DIR:=usrsctp
USRSCTP_FLAGS:=-DINET -DINET6

SRCS=$(shell printf "%s " src/*.cpp)
OBJS=$(subst .cpp,.o,$(SRCS))

all: $(NAME).a $(NAME).so

%.o: %.cpp
	$(CXX) $(INCLUDES) $(CPPFLAGS) $(USRSCTP_FLAGS) -I. -MMD -MP -o $@ -c $<

-include $(subst .o,.d,$(OBJS))

$(NAME).a: $(OBJS)
	$(AR) crf $@ $(OBJS)

$(NAME).so: libusrsctp.a $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS) -shared libusrsctp.a

clean:
	$(RM) src/*.o src/*.d

dist-clean: clean
	$(RM) $(NAME).a
	$(RM) $(NAME).so
	$(RM) libusrsctp.a
	$(RM) src/*~

libusrsctp.a:
	cd $(USRSCTP_DIR) && ./bootstrap && CFLAGS="-fPIC -Wno-address-of-packed-member" ./configure --enable-static && make
	cp $(USRSCTP_DIR)/usrsctplib/.libs/libusrsctp.a .
