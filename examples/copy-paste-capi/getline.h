// Simple POSIX getline() implementation
// This code is public domain

#include <malloc.h>
#include <stddef.h>
#include <stdio.h>

int getline(char **lineptr, size_t *n, FILE *stream) {
	if (!lineptr || !stream || !n)
		return -1;

	int c = getc(stream);
	if (c == EOF)
		return -1;

	if (!*lineptr) {
		*lineptr = malloc(128);
		if (!*lineptr)
			return -1;

		*n = 128;
	}

	int pos = 0;
	while(c != EOF) {
        if (pos + 1 >= *n) {
            size_t new_size = *n + (*n >> 2);
			if (new_size < 128)
				new_size = 128;

			char *new_ptr = realloc(*lineptr, new_size);
			if (!new_ptr)
				return -1;

			*n = new_size;
            *lineptr = new_ptr;
        }

        ((unsigned char *)(*lineptr))[pos ++] = c;
		if (c == '\n')
			break;

		c = getc(stream);
    }

    (*lineptr)[pos] = '\0';
    return pos;
}
