CC=cc
CFLAGS=-c -Wall -Wextra -std=gnu99
SOURCES=compiler.c
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=colorforth

all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

.c.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf $(EXECUTABLE) $(OBJECTS)
