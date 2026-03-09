CC = gcc

ifeq ($(OS),Windows_NT)
    CFLAGS  = -std=c99 -Wall -Wextra -O3 -march=native
    LDFLAGS = -lm
    TARGET  = pi.exe
    RM      = del /F /Q
else
    CFLAGS  = -std=c99 -Wall -Wextra -O3 -march=native -D_DEFAULT_SOURCE
    LDFLAGS = -lpthread -lm
    TARGET  = pi
    RM      = rm -f
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): test.c thread_pool.c thread_pool.h thread_pool_wrapper.h
	$(CC) $(CFLAGS) -o $@ test.c $(LDFLAGS)

clean:
	$(RM) $(TARGET)
