CC       = cc
CFLAGS   = -std=c11 -O2 -Wall -Wextra -Wpedantic \
           -Ithird_party -Ithird_party/glfw/include -Isrc
LDFLAGS  = -Lthird_party/glfw/build/src
LDLIBS   = -lglfw3 -lGL -lX11 -lpthread -lm -ldl -lwayland-client

SRC      = src/main.c src/window.c src/camera.c src/font.c src/text.c src/render.c src/fx.c src/segment.c src/linebreak.c src/source.c src/markdown.c src/image.c src/svg.c src/geom.c src/scene.c
OBJ      = $(SRC:.c=.o)
BIN      = utterance

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	ln -sf $(BIN) $(DESTDIR)$(BINDIR)/ut

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN) $(DESTDIR)$(BINDIR)/ut

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean install uninstall
