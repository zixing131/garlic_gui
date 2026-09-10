#ifndef GARLIC_JAVA_STRING_H
#define GARLIC_JAVA_STRING_H
/* Return a pool-allocated escaped string body from JVM/DEX Modified UTF-8.
 * Never decode encoded NUL into a C-string terminator. */
char *java_escape_string(const char *text);
#endif
