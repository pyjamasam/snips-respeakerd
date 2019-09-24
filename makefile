CC=gcc
CXX=g++
RM=rm -f

CPPFLAGS=-I/usr/include/respeaker/ -I/usr/include/libevdev-1.0/libevdev/ -Wno-psabi -fPIC -std=c++11 -fpermissive
LDFLAGS=
LDLIBS=-lsndfile -lrespeaker -levdev -lmosquitto

SRCS=snips-respeakerd.cc
OBJS=$(subst .cc,.o,$(SRCS))

all: snips-respeakerd

snips-respeakerd: $(OBJS)
	$(CXX) $(LDFLAGS) -o snips-respeakerd $(OBJS) $(LDLIBS)

.PHONY: clean
clean:
	$(RM) $(OBJS)

.PHONY: distclean
distclean: clean
	$(RM) snips-respeakerd