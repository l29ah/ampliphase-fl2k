CFLAGS += -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror
ifdef DEBUG
	CFLAGS+=-ggdb3 -DDEBUG -O0
else
	CFLAGS+=-DNDEBUG -O2
endif

CFLAGS += $(shell pkg-config libosmo-fl2k --cflags)
LDLIBS := $(shell pkg-config libosmo-fl2k --libs) -lm

EXE = ampliphase-fl2k

all: main-build

main-build: astyle
	$(MAKE) --no-print-directory $(EXE)

.PHONY:	clean astyle

clean:
	rm -rf *.o *.d $(EXE)

astyle:
	astyle --style=linux --indent=tab --unpad-paren --pad-header --pad-oper *.c
