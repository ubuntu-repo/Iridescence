CC=gcc
CFLAGS=-c -Wall -Wextra -std=gnu99 $(shell sdl2-config --cflags)
LDFLAGS=-lSDL2 -lSDL2_ttf $(shell sdl2-config --libs)
SOURCES=compiler.c editor.c
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=iridescence

all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

.c.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf $(EXECUTABLE) $(OBJECTS)
