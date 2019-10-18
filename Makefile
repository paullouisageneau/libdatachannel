# libdatachannel

NAME=libdatachannel
CXX=$(CROSS)g++
AR=$(CROSS)ar
RM=rm -f
CPPFLAGS=-O2 -pthread -fPIC -Wall -Wno-address-of-packed-member
CXXFLAGS=-std=c++17
LDFLAGS=-pthread
LDLIBS= -lgnutls $(shell pkg-config --libs glib-2.0 gobject-2.0 nice)
INCLUDES=-Iinclude/rtc -I$(USRSCTP_DIR)/usrsctplib $(shell pkg-config --cflags glib-2.0 gobject-2.0 nice)

USRSCTP_DIR:=usrsctp

SRCS=$(shell printf "%s " src/*.cpp)
OBJS=$(subst .cpp,.o,$(SRCS))

all: $(NAME).a $(NAME).so tests

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) -MMD -MP -o $@ -c $<

test/%.o: test/%.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -Iinclude -MMD -MP -o $@ -c $<

-include $(subst .cpp,.d,$(SRCS))

$(NAME).a: $(OBJS)
	$(AR) crf $@ $(OBJS)

$(NAME).so: libusrsctp.a $(OBJS)
	$(CXX) $(LDFLAGS) -shared -o $@ $(OBJS) $(LDLIBS) libusrsctp.a

tests: $(NAME).a test/main.o
	$(CXX) $(LDFLAGS) -o $@ test/main.o $(LDLIBS) $(NAME).a libusrsctp.a

clean:
	-$(RM) include/rtc/*.d *.d
	-$(RM) src/*.o src/*.d
	-$(RM) test/*.o test/*.d

dist-clean: clean
	-$(RM) $(NAME).a
	-$(RM) $(NAME).so
	-$(RM) libusrsctp.a
	-$(RM) tests
	-$(RM) include/*~
	-$(RM) src/*~
	-$(RM) test/*~
	-cd $(USRSCTP_DIR) && make clean

libusrsctp.a:
	cd $(USRSCTP_DIR) && \
		./bootstrap && \
		./configure --enable-static --disable-debug CFLAGS="$(CPPFLAGS)" && \
		make
	cp $(USRSCTP_DIR)/usrsctplib/.libs/libusrsctp.a .

