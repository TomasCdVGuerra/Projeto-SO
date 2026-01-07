#include <stdlib.h>
#include <stdio.h> //snprintf
#include <unistd.h>
#include <stdarg.h>
#include <time.h>

FILE *debugfile;

void open_debug_file(char *filename)
{
    debugfile = fopen(filename, "w");
    if (!debugfile)
    {
        return;
    }
    // Buffered logging: avoids expensive fflush() on every debug() call.
    setvbuf(debugfile, NULL, _IOLBF, 0);
}

void close_debug_file()
{
    if (debugfile)
    {
        fclose(debugfile);
        debugfile = NULL;
    }
}

void debug(const char *format, ...)
{
    if (!debugfile)
    {
        return;
    }
    va_list args;
    va_start(args, format);
    vfprintf(debugfile, format, args);
    va_end(args);
}

void sleep_ms(int milliseconds)
{
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}