#ifndef XNET_CANOPEN_LOGGING_CONFIG_H
#define XNET_CANOPEN_LOGGING_CONFIG_H

/*
 * Set the absolute log file path here, or define the same macro at compile time.
 * The deployment account must be able to create/write its parent directory.
 * All handles in this library instance share this file and its rotated backups.
 */
#ifndef XNET_CANOPEN_LOG_PATH
#ifdef _WIN32
#define XNET_CANOPEN_LOG_PATH "C:\\ProgramData\\xnet_canopen\\xnet_canopen.log"
#else
#define XNET_CANOPEN_LOG_PATH "/var/local/natinst/log/xnet_canopen.log"
#endif
#endif

#endif
