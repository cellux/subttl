CXX      = $(shell fltk-config --cxx)
DEBUG    = -g
CXXFLAGS = $(shell fltk-config --cxxflags) -I.
LDFLAGS  = $(shell fltk-config --ldflags) -lsndfile -lportaudio
LINK     = $(CXX)

TARGET = subttl
OBJS = subttl.o
SRCS = subttl.cxx

.SUFFIXES: .o .cxx
%.o: %.cxx
	$(CXX) $(CXXFLAGS) $(DEBUG) -c $<

$(TARGET): $(OBJS)
	$(LINK) -o $(TARGET) $(OBJS) $(LDFLAGS)

.PHONY: clean
clean:
	rm -f *.o 2> /dev/null
	rm -f $(TARGET) 2> /dev/null
