// mac_bootlog.h -- the log that survives a freeze (mac_bootlog.c)
#ifndef MAC_BOOTLOG_H
#define MAC_BOOTLOG_H

int		Sys_BootLogOpen( const char *directory );
void	Sys_BootLogWrite( const char *text );
void	Sys_BootLogFlush( void );
void	Sys_BootLogUnlock( void );
void	Sys_BootLogClose( void );

#endif
