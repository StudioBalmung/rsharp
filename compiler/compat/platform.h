/* rsharp/compiler/compat/platform.h */
#pragma once

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <process.h>
#define PLATFORM_MKDIR(path, mode) _mkdir(path)
#define PLATFORM_ISATTY(fd) _isatty(fd)
#define PLATFORM_CHMOD(path, mode) _chmod(path, mode)
#define PLATFORM_GETPID() _getpid()
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif
#else
#include <sys/stat.h>
#include <unistd.h>
#define PLATFORM_MKDIR(path, mode) mkdir(path, mode)
#define PLATFORM_ISATTY(fd) isatty(fd)
#define PLATFORM_CHMOD(path, mode) chmod(path, mode)
#define PLATFORM_GETPID() getpid()
#endif
