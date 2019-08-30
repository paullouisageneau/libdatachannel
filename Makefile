# libdatachannel

NAME=libdatachannel
CXX=$(CROSS)g++
AR=$(CROSS)ar
RM=rm -f
CPPFLAGS=-O2 -pthread -fPIC -Wall -Wno-address-of-packed-member
CXXFLAGS=-std=c++17
LDFLAGS=-pthread
LDLIBS=$(shell pkg-config --libs glib-2.0 gobject-2.0 nice) -lgnutls
INCLUDES=$(shell pkg-config --cflags glib-2.0 gobject-2.0 nice) -I$(USRSCTP_DIR)/usrsctplib

USRSCTP_DIR:=usrsctp
USRSCTP_DEFINES:=-DINET -DINET6

SRCS=$(shell printf "%s " src/*.cpp)
OBJS=$(subst .cpp,.o,$(SRCS))

all: $(NAME).a $(NAME).so tests

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) $(USRSCTP_DEFINES) -MMD -MP -o $@ -c $<

test/%.o: test/%.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -Isrc -MMD -MP -o $@ -c $<

-include $(subst .o,.d,$(OBJS))

$(NAME).a: $(OBJS)
	$(AR) crf $@ $(OBJS)

$(NAME).so: libusrsctp.a $(OBJS)
	$(CXX) $(LDFLAGS) -shared -o $@ $(OBJS) $(LDLIBS) libusrsctp.a

tests: $(NAME).a test/main.o
	$(CXX) $(LDFLAGS) -o $@ test/main.o $(LDLIBS) $(NAME).a libusrsctp.a

clean:
	-$(RM) src/*.o src/*.d
	-$(RM) test/*.o test/*.d

dist-clean: clean
	-$(RM) $(NAME).a
	-$(RM) $(NAME).so
	-$(RM) libusrsctp.a
	-$(RM) tests
	-$(RM) src/*~
	-$(RM) test/*~
	-cd $(USRSCTP_DIR) && make clean

libusrsctp.a:
	cd $(USRSCTP_DIR) && \
		./bootstrap && \
		./configure --enable-static --disable-debug CFLAGS="$(CPPFLAGS)" && \
		$(MAKE)
	cp $(USRSCTP_DIR)/usrsctplib/.libs/libusrsctp.a .

