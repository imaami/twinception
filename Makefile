CC := gcc
CFLAGS := -O3 -flto=auto -march=native -mtune=native -std=gnu23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
override LDLIBS = $(eval override LDLIBS := \
 $$(shell pkg-config --libs libcurl json-c))$(LDLIBS)
override CPPFLAGS = $(eval override CPPFLAGS := \
 $$(shell pkg-config --cflags libcurl json-c))$(CPPFLAGS)

override src := \
  src/app.c     \
  src/buf.c     \
  src/http.c    \
  src/loop.c    \
  src/main.c    \
  src/sse.c

override obj := $(src:=.o)
override dep := $(src:=.d)

twinception: $(obj)
	$(CC) $(CFLAGS) -MMD -o $@ $^ $(LDLIBS)

%.c.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -c -o $@ $<

check: twinception
	python3 tests/check.py ./twinception

clean:
	$(RM) twinception $(obj) $(dep)

.PHONY: check clean

-include $(dep)
