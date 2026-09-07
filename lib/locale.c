#include <locale.h>

static char _dot[] = ".";
static char _empty[] = "";

static struct lconv _c_lconv = {
    .decimal_point = _dot,
    .thousands_sep = _empty,
    .grouping = _empty,
    .int_curr_symbol = _empty,
    .currency_symbol = _empty,
    .mon_decimal_point = _empty,
    .mon_thousands_sep = _empty,
    .mon_grouping = _empty,
    .positive_sign = _empty,
    .negative_sign = _empty,
    .int_frac_digits = (char)0x7f,
    .frac_digits = (char)0x7f,
    .p_cs_precedes = (char)0x7f,
    .p_sep_by_space = (char)0x7f,
    .n_cs_precedes = (char)0x7f,
    .n_sep_by_space = (char)0x7f,
    .p_sign_posn = (char)0x7f,
    .n_sign_posn = (char)0x7f,
    .int_p_cs_precedes = (char)0x7f,
    .int_n_cs_precedes = (char)0x7f,
    .int_p_sep_by_space = (char)0x7f,
    .int_n_sep_by_space = (char)0x7f,
    .int_p_sign_posn = (char)0x7f,
    .int_n_sign_posn = (char)0x7f,
};

static char _locale_name[] = "C";

struct lconv *localeconv(void)
{
    return &_c_lconv;
}

char *setlocale(int category, const char *locale)
{
    (void)category;

    if (!locale)
        return _locale_name;

    if (locale[0] == '\0')
        return _locale_name;

    if (locale[0] == 'C' && locale[1] == '\0')
        return _locale_name;

    if (locale[0]=='P' && locale[1]=='O' && locale[2]=='S' &&
        locale[3]=='I' && locale[4]=='X' && locale[5]=='\0')
        return _locale_name;

    return (char*)0;
}