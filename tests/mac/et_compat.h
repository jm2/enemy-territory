/* What ET's engine differs in from Quake III for these Quake III tests. ET's q_shared.h bans
 * vsnprintf, and Q_vsnprintf lives in its platform code rather than q_shared.c. */
#ifndef ET_TEST_COMPAT_H
#define ET_TEST_COMPAT_H
#include <stdio.h>
#undef vsnprintf
#undef _vsnprintf
#ifndef ET_TEST_NO_Q_VSNPRINTF
int Q_vsnprintf( char *dest, int size, const char *fmt, va_list argptr ) {
	return vsnprintf( dest, size, fmt, argptr );
}
#endif
/* ET receives into exactly MAX_MSGLEN, as ET: Legacy's client does */
#ifndef MAX_MSGLEN_BUF
#define MAX_MSGLEN_BUF MAX_MSGLEN
#endif
#endif
