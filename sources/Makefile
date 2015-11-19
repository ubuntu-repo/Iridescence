CC=cc
CFLAGS=-c -m32 -Wall -Wextra -std=gnu99
LDFLAGS=-m32
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
