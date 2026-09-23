# Makefile — Mini-UnionFS
#
# Build with: make
# Clean with: make clean
# Debug build with sanitizers: make debug
#
# Requires libfuse3 development headers.
# If fuse3 is installed in a non-standard location, set PKG_CONFIG_PATH:
#   PKG_CONFIG_PATH=/path/to/fuse3/lib/pkgconfig make

CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++20 $(shell pkg-config --cflags fuse3)
LDFLAGS  = $(shell pkg-config --libs fuse3)

SRCDIR   = src
SRCS     = $(SRCDIR)/main.cpp $(SRCDIR)/path.cpp $(SRCDIR)/rw_ops.cpp $(SRCDIR)/del_ops.cpp
OBJS     = $(SRCS:.cpp=.o)
TARGET   = mini_unionfs

.PHONY: all clean run debug

all: $(TARGET)

run: $(TARGET)
	./unionfs_cli.sh

debug: CXXFLAGS += -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
debug: clean $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/%.o: $(SRCDIR)/%.cpp $(SRCDIR)/unionfs.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
