all: kilo

kilo: kilo.o
	$(CC) -o "$@" $<

%.o: %.c
	$(CC) -c -o "$@" -Wall -Wextra -Werror -pedantic -std=c99 "$<"

clean:
	rm kilo.o kilo

.PHONY: all clean
