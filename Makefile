# Makefile for cross-compiling apaPID03 for SSC338Q

CC = arm-linux-gnueabihf-gcc
CFLAGS = -Wall -O2
TARGET = apaPID03
SRCS = apaPID03.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

strip:
	arm-linux-gnueabihf-strip $(TARGET)

clean:
	rm -f $(TARGET)
