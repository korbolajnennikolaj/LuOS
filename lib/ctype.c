#include <ctype.h>

int _ctype_isdigit (int c) { return isdigit(c); }
int _ctype_isupper (int c) { return isupper(c); }
int _ctype_islower (int c) { return islower(c); }
int _ctype_isalpha (int c) { return isalpha(c); }
int _ctype_isalnum (int c) { return isalnum(c); }
int _ctype_isspace (int c) { return isspace(c); }
int _ctype_isxdigit(int c) { return isxdigit(c); }
int _ctype_isprint (int c) { return isprint(c); }
int _ctype_isgraph (int c) { return isgraph(c); }
int _ctype_iscntrl (int c) { return iscntrl(c); }
int _ctype_ispunct (int c) { return ispunct(c); }
int _ctype_isblank (int c) { return isblank(c); }
int _ctype_tolower (int c) { return tolower(c); }
int _ctype_toupper (int c) { return toupper(c); }