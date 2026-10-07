# Recursive (`=`) and conditional assignments (`?=`) capture env variables,
# simple assignments (`:=`) without `override` only command line arguments.
CC     := gcc
CFLAGS := -O3 -flto=auto -march=native -mtune=native -std=gnu23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow

# This lazy-eval oneshot assignment trick prevents unnecessary shell churn;
# each `pkg-config` command runs only once when the variable is referenced.
override LDLIBS = $(eval override LDLIBS := \
 $$(shell pkg-config --libs libcurl json-c))$(LDLIBS)
override CPPFLAGS = $(eval override CPPFLAGS := \
 $$(shell pkg-config --cflags libcurl json-c))$(CPPFLAGS)

override COMMON := \
  src/buf.c     \
  src/http.c    \
  src/loop.c

override TWINCEPTION := \
  src/app.c     \
  src/main.c    \
  src/sse.c

override ASKAI := \
  src/ai.c      \
  src/askai.c   \
  src/httpd.c   \
  src/jb.c      \
  src/mcp.c

override SRC := $(COMMON) $(TWINCEPTION) $(ASKAI)
override OBJ := $(SRC:=.o)
override DEP := $(SRC:=.d)

all: twinception askai

twinception: $(TWINCEPTION:=.o) $(COMMON:=.o)
	$(CC) $(CFLAGS) -MMD -o $@ $^ $(LDLIBS)

askai: $(ASKAI:=.o) $(COMMON:=.o)
	$(CC) $(CFLAGS) -MMD -o $@ $^ $(LDLIBS)

%.c.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -c -o $@ $<

check: twinception askai
	python3 tests/check.py ./twinception
	python3 tests/askai.py ./askai

purge: | clean
	$(RM) $(DEP)

clean:
	$(RM) twinception askai $(OBJ)

.PHONY: all check clean purge

-include $(DEP)
