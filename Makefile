CC       = cc
CFLAGS   = -std=c11 -O2 -Wall -Wextra -Wpedantic \
           -Ithird_party -Ithird_party/glfw/include -Isrc
LDFLAGS  = -Lthird_party/glfw/build/src
LDLIBS   = -lglfw3 -lGL -lX11 -lpthread -lm -ldl -lwayland-client

SRC      = src/main.c src/window.c src/camera.c src/font.c src/text.c src/render.c
OBJ      = $(SRC:.c=.o)
BIN      = utterance

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
