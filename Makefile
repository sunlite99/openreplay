CC=g++
SDK_PATH=../../include
CFLAGS=-Wno-multichar -Wall -Wextra -I $(SDK_PATH) -fno-rtti -g -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS
LDFLAGS=-lm -ldl -lpthread
FFCFLAGS=`pkg-config libavcodec libavformat libswscale libavutil --cflags` 
FFLDFLAGS=`pkg-config libavcodec libavformat libswscale libavutil --libs` -lavutil

all: raw_ingest ingest output preview_stream

sdl_gui: sdl_gui.cpp mmap_buffer.cpp ffwrapper.cpp
	$(CC) $(CFLAGS) $(FFCFLAGS) `sdl-config --cflags` -o sdl_gui sdl_gui.cpp ffwrapper.cpp mmap_buffer.cpp $(LDFLAGS) $(FFLDFLAGS) `sdl-config --libs` -lSDL_image -lSDL_ttf 

mjpeg_ingest: mjpeg_ingest.cpp mmap_buffer.cpp
	$(CC) -o mjpeg_ingest mjpeg_ingest.cpp mmap_buffer.cpp $(CFLAGS) $(LDFLAGS)

decklink_capture: decklink_capture.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp
	$(CC) -o decklink_capture decklink_capture.cpp mmap_buffer.cpp $(SDK_PATH)/DeckLinkAPIDispatch.cpp $(CFLAGS) $(LDFLAGS)

bmdplayoutd: bmdplayoutd.cpp ffwrapper.cpp mmap_buffer.cpp
	$(CC) -o bmdplayoutd bmdplayoutd.cpp ffwrapper.cpp mmap_buffer.cpp $(CFLAGS) $(LDFLAGS) $(LAVC_INCLUDES) $(LAVC_LIBS) $(FFCFLAGS) $(FFLDFLAGS)

clean:
	rm -f raw_ingest ingest output
