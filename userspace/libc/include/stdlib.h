/*
	Glidix Runtime

	Copyright (c) 2014-2015, Madd Games.
	All rights reserved.
	
	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:
	
	* Redistributions of source code must retain the above copyright notice, this
	  list of conditions and the following disclaimer.
	
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	
	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
	DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
	FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
	DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
	SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
	CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
	OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
	OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _STDLIB_H
#define _STDLIB_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	MB_CUR_MAX				1

/* implemented by the runtime */
void			abort(void);
int			atexit(void (*)(void));
int			atoi(const char*);
void			free(void*);
char*			getenv(const char*);
void*			malloc(size_t);
void*			realloc(void*, size_t);
void			exit(int);
void			_Exit(int);
void*			calloc(size_t nitems, size_t size);
char*			getenv(const char *name);
int			setenv(const char *name, const char *value, int update);
long			strtol(const char *str, char **str_end, int base);
long long		strtoll(const char *str, char **str_end, int base);
unsigned long		strtoul(const char *str, char **str_end, int base);
unsigned long long	strtoull(const char *str, char **str_end, int base);
int			atoi(const char *str);
long			atol(const char *str);
long long		atoll(const char *str);
void			qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
void*			bsearch(const void *key, const void *base, size_t num, size_t size, int (*cmp)(const void *, const void *));
int			wctomb(char *s, wchar_t wc);
int			mbtowc(wchar_t *pwc, const char *s, size_t n);

/* implemented by libglidix directly */
char*	realpath(const char*, char*);

#ifdef __cplusplus
}
#endif

#endif