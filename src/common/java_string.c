#include "java_string.h"
#include "mem_pool.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *java_escape_string(const char *text) {
    if (!text)
        text = "";
    size_t length = strlen(text);
    char *result = x_alloc(length * 6 + 1), *out = result;
    const unsigned char *p = (const unsigned char *)text;
    const unsigned char *end = p + length;
    const char *setting = getenv("GARLIC_ESCAPE_UNICODE");
    int escape_unicode = setting && strcmp(setting, "1") == 0;
    while (p < end) {
        const unsigned char *start = p;
        uint32_t cp = *p++;
        if (cp >= 0xc0 && cp <= 0xdf && end - p >= 1 && (p[0] & 0xc0) == 0x80) {
            cp = ((cp & 31) << 6) | (p[0] & 63);
            p++;
        } else if (cp >= 0xe0 && cp <= 0xef && end - p >= 2 && (p[0] & 0xc0) == 0x80 &&
                   (p[1] & 0xc0) == 0x80) {
            cp = ((cp & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
            p += 2;
        } else if (cp >= 0xf0 && cp <= 0xf4 && end - p >= 3 && (p[0] & 0xc0) == 0x80 &&
                   (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            cp = ((cp & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
            p += 3;
        } else if (cp >= 0x80) {
            out += sprintf(out, "\\u%04x", cp);
            continue;
        }
        switch (cp) {
        case '\n':
            *out++ = '\\';
            *out++ = 'n';
            continue;
        case '\r':
            *out++ = '\\';
            *out++ = 'r';
            continue;
        case '\t':
            *out++ = '\\';
            *out++ = 't';
            continue;
        case '\b':
            *out++ = '\\';
            *out++ = 'b';
            continue;
        case '\f':
            *out++ = '\\';
            *out++ = 'f';
            continue;
        case '"':
            *out++ = '\\';
            *out++ = '"';
            continue;
        case '\\':
            *out++ = '\\';
            *out++ = '\\';
            continue;
        }
        if (cp < 32 || (cp >= 0x7f && cp <= 0x9f) || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp == 0x2028 || cp == 0x2029 || (escape_unicode && cp > 127)) {
            if (cp > 0xffff) {
                cp -= 0x10000;
                out += sprintf(out, "\\u%04x\\u%04x", 0xd800 + (cp >> 10), 0xdc00 + (cp & 1023));
            } else
                out += sprintf(out, "\\u%04x", cp);
        } else if (cp < 128)
            *out++ = (char)cp;
        else {
            size_t bytes = (size_t)(p - start);
            memcpy(out, start, bytes);
            out += bytes;
        }
    }
    *out = 0;
    return result;
}
