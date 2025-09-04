CC := gcc

# Choose backend at compile time: BACKEND=sdl1 (default) or BACKEND=opengl
BACKEND ?= opengl

# Option to disable FFmpeg entirely (requires HLS streams with custom decoders)
NO_FFMPEG ?= 1

# Optimized compiler flags for maximum performance
CFLAGS := -O2 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions
CFLAGS += -Wall -Wextra -Iinclude -g

# Conditionally include FFmpeg libraries
ifeq ($(NO_FFMPEG),0)
    CFLAGS += $(shell pkg-config --cflags libavformat libavcodec libswscale libavutil sdl libcurl)
    LIBS := $(shell pkg-config --libs libavformat libavcodec libswscale libavutil sdl libcurl)
else
    CFLAGS += $(shell pkg-config --cflags sdl libcurl) -DNO_FFMPEG
    LIBS := $(shell pkg-config --libs sdl libcurl)
endif

# Set debug or release mode
DEBUG ?= 0
ifeq ($(DEBUG),1)
    CFLAGS += -DDEBUG -O1  # Light optimization for debugging
    LDFLAGS := -O1
else
    CFLAGS += -DNDEBUG  # Disable debug assertions for performance
    LDFLAGS := -flto -O3  # Link-time optimization
endif

# When NO_FFMPEG is enabled, automatically enable custom decoders
ifeq ($(NO_FFMPEG),1)
    USE_SIMPLE_H264 := 1
    USE_MPEG4 := 1
endif

ifeq ($(BACKEND),opengl)
	BACK_SRC := src/gapis/gopengl.c
	EXTRA_LIBS := -lGL
	CFLAGS += -DBACKEND_OPENGL
else
	BACK_SRC := src/gapis/gsdl1.c
	EXTRA_LIBS :=
endif

SRCS := src/main.c src/resolver/twitch.c src/memory_pool.c src/dmux/hls/hls_demuxer.c src/dmux/hls/playlist_parser.c $(BACK_SRC)

# Optional: include the simple H.264 decoder sources when requested
USE_SIMPLE_H264 ?= 0
ifeq ($(USE_SIMPLE_H264),1)
    SIMPLE_H264_SRCS := $(wildcard src/codecs/simple_h264/*.c)
    SRCS += $(SIMPLE_H264_SRCS)
    # Make the macro available to sources (including main.c)
    CFLAGS += -DUSE_SIMPLE_H264
endif

# Optional: include the MPEG-4 decoder sources when requested
USE_MPEG4 ?= 0
ifeq ($(USE_MPEG4),1)
    MPEG4_SRCS := $(wildcard src/codecs/mpeg4/*.c)
    SRCS += $(MPEG4_SRCS)
    # Make the macro available to sources (including main.c)
    CFLAGS += -DUSE_MPEG4
endif

OBJS := $(SRCS:.c=.o)
TARGET := bin/app

# Compile-time frameskip amount (number of frames to consider before aggressive skipping)
FRAMESKIP ?= 3

# Memory optimization flags
MEMORY_OPTS := -DMALLOC_TRIM_THRESHOLD=16384   # Trim malloc after just 16KB (extremely aggressive)
MEMORY_OPTS += -DMMAP_THRESHOLD=32768          # Use mmap for allocations >32KB
MEMORY_OPTS += -DMALLOC_MMAP_MAX_=0            # Disable mmap for small allocations
MEMORY_OPTS += -DMALLOC_TOP_PAD_=1024          # Minimal padding at top of heap
MEMORY_OPTS += -DMALLOC_ARENA_MAX=1            # Single arena (big memory saving)
MEMORY_OPTS += -DMALLOC_MMAP_THRESHOLD_MAX=131072 # Upper limit on mmap threshold

# Memory optimized build mode
MEMORY_OPTIMIZED ?= 0
ifeq ($(MEMORY_OPTIMIZED),1)
    CFLAGS += $(MEMORY_OPTS) -Os -ffunction-sections -fdata-sections
    CFLAGS += -fno-common -fmerge-all-constants -fno-asynchronous-unwind-tables
    CFLAGS += -fno-stack-protector -fomit-frame-pointer -fno-exceptions
    CFLAGS += -fno-unwind-tables -fno-ident
    LDFLAGS += -Wl,--gc-sections -Wl,--strip-all -Wl,--as-needed
    LDFLAGS += -Wl,--build-id=none -Wl,--hash-style=gnu
    CPPFLAGS += -DMINIMAL_MEMORY_BUFFERS -DREDUCED_CACHE_SIZE=1
endif

# Propagate frameskip amount to compiler
CFLAGS += -DFRAMESKIP_AMOUNT=$(FRAMESKIP)

.PHONY: all clean noskip smooth

all: $(TARGET)

# Build with frameskip disabled
noskip: CFLAGS += -DDISABLE_FRAMESKIP
noskip: $(TARGET)

# Build with memory optimizations for low-memory environments
memory: MEMORY_OPTIMIZED=1
memory: $(TARGET)

bin:
	mkdir -p bin

$(TARGET): bin $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS) $(EXTRA_LIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Explicit rule for simple h264 objects
src/codecs/simple_h264/%.o: src/codecs/simple_h264/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf bin src/**/*.o src/*.o
