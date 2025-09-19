// string.c - String and memory functions
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "string.h"
#include "console.h"
#include <stdint.h>
#include <limits.h>

// INTERNAL STRUCTURE DEFINITIONS
// 

struct vsnprintf_state {
    char * pos;
    size_t rem;
};

// INTERNAL FUNCTION DECLARATIONS
// 

static void vsnprintf_putc(char c, void * aux);

static size_t format_int (
    void (*putcfn)(char, void*), void * aux,
    uintmax_t val, unsigned int base, int zpad, unsigned int len);

static size_t format_str (
    void (*putcfn)(char, void*), void * aux,
    const char * s, unsigned int len);

size_t format_char (
    void (*putcfn)(char, void*), void * aux,
    int c, unsigned int len);

// EXPORTED FUNCTION DEFINITIONS
// 

int strcmp(const char * s1, const char * s2) {
    // A null pointer compares before any non-null pointer

    if (s1 == NULL || s2 == NULL) {
        if (s1 == NULL)
            return (s2 == NULL) ? 0 : -1;
        else
            return 1;
    }

    // Find first non-matching character (or '\0')

    while (*s1 == *s2 && *s1 != '\0') {
        s1 += 1;
        s2 += 1;
    }

    if (*s1 < *s2)
        return -1;
    else if (*s1 > *s2)
        return 1;
    else // null char
        return 0;
}

int strncmp(const char * s1, const char * s2, size_t n) {
  while (n != 0 && *s1 != '\0' && *s1 == *s2) {
    s1 += 1;
    s2 += 1;
    n -= 1;
  }

  if (n != 0)
    return (unsigned int)*s1 - (unsigned int)*s2;
  else
    return 0;
}

size_t strlen(const char * s) {
    const char * p = s;

    if (s == NULL)
        return 0;

    while (*p != '\0')
        p += 1;
    
    return (p - s);
}

char * strncpy(char *dst, const char *src, size_t n) {
    char * orig_dst = dst;

    while (n != 0 && *src != '\0') {
        *dst = *src;
        src += 1;
        dst += 1;
        n -= 1;
    }

    if (n != 0)
        *dst = '\0';
    
    return orig_dst;
}

char * strchr(const char * s, int c) {
    while (*s != '\0') {
        if (*s == c)
            return (char*)s;
        s += 1;
    }

    return NULL;
}

char * strrchr(const char * s, int c) {
    char * p = NULL;

    while (*s != '\0') {
        if (*s == c)
            p = (char*)s;
        s += 1;
    }

    return p;
}

void * memset(void * s, int c, size_t n) {
  char * p = s;

  while (n != 0) {
    *p++ = c;
    n -= 1;
  }

  return s;
}

void * memcpy(void * restrict dst, const void * restrict src, size_t n) {
    const char * q = src;
    char *p = dst;
    while (n != 0) {
        // kprintf("buf_address_dst: %p\n", p);
        // kprintf("buf_address_src: %p\n", q);
        *p = *q;
        p += 1;
        q += 1;
        n -= 1;
    }

    return dst;
}

int memcmp(const void * p1, const void * p2, size_t n) {
    const uint8_t * u = p1;
    const uint8_t * v = p2;

    while (n != 0) {
        if (*u != *v)
            return (*u - *v);
        u += 1;
        v += 1;
        n -= 1;
    }

    return 0;
}

unsigned long strtoul(const char * str, char ** endptr, int base) {
    unsigned long val = 0;
    int neg = 0;
    char c;

    // strtoul not implemented for base 0 or bases greater than 10

    if (str == NULL || base <= 1 || 10 < base) {        
        if (endptr != NULL)
            *endptr = NULL;
        return -1UL;
    }

    if (*str == '-') {
        str += 1;
        neg = 1;
    } else if (*str == '+')
        str += 1;

    if (base <= 10) {
        for (;;) {
            c = *str;
            if (c < '0' || '0' + base <= c) {
                if (endptr != NULL)
                    *endptr = (char*)str;
                return neg ? -val : val;
            }
            
            val *= base;
            val += c - '0';
            str += 1;
        }
    }

    return val;
}

size_t snprintf(char * buf, size_t bufsz, const char * fmt, ...) {
    va_list ap;
    size_t n;

    va_start(ap, fmt);
    n = vsnprintf(buf, bufsz, fmt, ap);
    va_end(ap);
    return n;
}

size_t vsnprintf(char * buf, size_t bufsz, const char * fmt, va_list ap) {
    struct vsnprintf_state state;
    size_t n;

    state.pos = buf;
    state.rem = bufsz;

    n = vgprintf(vsnprintf_putc, &state, fmt, ap);

    if (state.rem != 0)
        *state.pos = '\0';

    return n;
}

size_t vgprintf (
    void (*putcfn)(char, void*), void * aux,
    const char * fmt, va_list ap)
{
    const char * p;
    size_t nout;
    int zpad;
    unsigned int len;
    unsigned int lcnt;
    intmax_t ival;
    int base;

    p = fmt;
    nout = 0;

    while (*p != '\0') {
        if (*p == '%') {
            p += 1;

            // Parse width specifier
            
            zpad = (*p == '0');
            len = 0;

            while ('0' <= *p && *p <= '9') {
                len = 10 * len + (*p - '0');
                p += 1;
            }

            // Check for l and other prefixes prefix

            lcnt = 0;
            while (*p == 'l') {
                lcnt += 1;
                p += 1;
            }

            if (*p == 'z') {
                if (sizeof(size_t) == sizeof(int))
                    lcnt = 0;
                if (sizeof(size_t) == sizeof(long))
                    lcnt = 1;
                if (sizeof(size_t) == sizeof(long long))
                    lcnt = 2;
                p += 1;
            } else if (*p == 'j') {
                if (sizeof(intmax_t) == sizeof(int))
                    lcnt = 0;
                if (sizeof(intmax_t) == sizeof(long))
                    lcnt = 1;
                if (sizeof(intmax_t) == sizeof(long long))
                    lcnt = 2;
                p += 1;
            }

            // Parse format specifier character

            switch (*p) {
            case 'd':
            case 'i':
                if (lcnt > 0)
                    if (lcnt > 1)
                        ival = va_arg(ap, long long);
                    else
                        ival = va_arg(ap, long);
                else
                    ival = va_arg(ap, int);
                
                if (ival < 0) {
                    putcfn('-', aux);
                    nout += 1;

                    ival = -ival;
                    if (len > 0)
                        len -= 1;
                }

                nout += format_int(putcfn, aux, ival, 10, zpad, len);
                break;

            case 'u':
            case 'x':
                if (lcnt > 0)
                    if (lcnt > 1)
                        ival = va_arg(ap, unsigned long long);
                    else
                        ival = va_arg(ap, unsigned long);
                else
                    ival = va_arg(ap, unsigned int);
                
                if (*p == 'x')
                    base = 16;
                else
                    base = 10;
                
                nout += format_int(putcfn, aux, ival, base, zpad, len);
                break;
            
            case 's':
                nout += format_str(putcfn, aux, va_arg(ap, char *), len);
                break;
            
            case 'c':
                nout += format_char(putcfn, aux, va_arg(ap, int), len);
                break;
            
            case 'p':
                ival = (uintptr_t)va_arg(ap, void *);
                nout += format_str(putcfn, aux, "0x", 2);
                nout += format_int(putcfn, aux, ival, 16, zpad, len);
                break;
            
            default:
                putcfn('%', aux);
                nout += 1;

                if (*p < ' ' || 0x7f <= *p)
                    putcfn('?', aux);
                else
                    putcfn(*p, aux);
                nout += 1;

                if (*p == '\0')
                    p -= 1;
                break;
            }
        
        } else {
            putcfn(*p, aux);
            nout += 1;
        }

        p += 1;
    }

    return nout;
}

// INTERNAL FUNCTION DEFINITIONS
// 

void vsnprintf_putc(char c, void * aux) {
    struct vsnprintf_state * state = aux;

    if (state->rem <= 1)
        return;

    *state->pos = c;
    state->pos += 1;
    state->rem -= 1;
}

size_t format_int (
    void (*putcfn)(char, void*), void * aux,
    uintmax_t val, unsigned int base, int zpad, unsigned int len)
{
    char buf[40];
    size_t nout;
    char * p;
    int d;

    nout = 0;
    p = buf + sizeof(buf);

    // Write formatted integer to buf from end

    do {
        d = val % base;
        val /= base;
        p -= 1;

        if (d < 10)
            *p = '0' + d;
        else
            *p = 'a' + d - 10;
    } while (val != 0);

    // Write padding if needed

    while (buf + sizeof(buf) - p < len) {
        if (zpad)
            putcfn('0', aux);
        else
            putcfn(' ', aux);
        nout += 1;
        len -= 1;
    }
    
    // Write formatted integer in buf.

    while (p < buf + sizeof(buf)) {
        putcfn(*p, aux);
        nout += 1;
        p += 1;
    }

    return nout;
}

size_t format_str (
    void (*putcfn)(char, void*), void * aux,
    const char * s, unsigned int len)
{
    size_t nout = 0;

    if (s == NULL)
        s = "(null)";

    while (*s != '\0') {
        putcfn(*s, aux);
        nout += 1;
        s += 1;
    }

    if (nout < len) {
        len -= nout;
        nout += len;

        while (len > 0) {
            putcfn(' ', aux);
            len -= 1;
        }
    }

    return nout;
}

size_t format_char (
    void (*putcfn)(char, void*), void * aux,
    int c, unsigned int len)
{
    size_t nout = 0;

    if (len <= 0)
        len = 1;
    
    nout += len;

    while (len > 1) {
        putcfn(' ', aux);
        len -= 1;
    }

    putcfn(c, aux);

    return nout;
}