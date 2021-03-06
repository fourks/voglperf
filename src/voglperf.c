/**************************************************************************
 *
 * Copyright 2013-2014 RAD Game Tools and Valve Software
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 **************************************************************************/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <syslog.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/msg.h>

#define __USE_GNU
#include <dlfcn.h>
#include <errno.h>

#include <GL/glx.h>

#include "SDL2/SDL_timer.h"
#include "SDL2/SDL_stdinc.h"
#include "SDL2/SDL_loadso.h"

#include "voglperf.h"

#define OS_POSIX 
#include "eintr_wrapper.h"

#define VOGL_API_EXPORT __attribute__((visibility("default")))

//----------------------------------------------------------------------------------------------------------------------
// globals
//----------------------------------------------------------------------------------------------------------------------
int g_showfps = 0;
int g_verbose = 0;

// If we write 4000 frametimes of '0.25 ', that would be 20,000 bytes. So 32k should be enough to
// handle a full second of reasonable frametimes.
int g_logfile_buf_len = 0;
char g_logfile_buf[32 * 1024];
int g_logfile_fd = -1;

int g_msqid = -1;

__attribute__((destructor)) static void vogl_perf_destructor_func();

#define SDL_X11_SYM(rc, fn, params, args, ret) \
    typedef rc(*SDL_DYNX11FN_##fn) params;     \
    SDL_DYNX11FN_##fn X11_##fn;

SDL_X11_SYM(XFontStruct *, XLoadQueryFont, (Display *a, _Xconst char *b), (a, b), return)
SDL_X11_SYM(GC, XCreateGC, (Display *a, Drawable b, unsigned long c, XGCValues *d), (a, b, c, d), return)
SDL_X11_SYM(int, XDrawString, (Display *a, Drawable b, GC c, int d, int e, _Xconst char *f, int g), (a, b, c, d, e, f, g), return)

typedef const GLubyte *(*GLAPIENTRY glGetString_func_ptr_t)(GLenum name);
typedef Bool (*glXMakeCurrent_func_ptr_t)(Display *dpy, GLXDrawable drawable, GLXContext ctx);
typedef void (*glXSwapBuffers_func_ptr_t)(Display *dpy, GLXDrawable drawable);
typedef void *(*dlopen_func_ptr_t)(const char *pFile, int mode);

// Use get_glinfo() to get gl/vendor/renderer/version associated with dpy+drawable
typedef struct glinfo_cache_t
{
    Display *dpy;
    GLXDrawable drawable;

    GC gc;
    GLXContext ctx;

    int glstrings_valid;
    const GLubyte *vendor;   // GL_VENDOR
    const GLubyte *renderer; // GL_RENDERER
    const GLubyte *version;  // GL_VERSION
} glinfo_cache_t;

//----------------------------------------------------------------------------------------------------------------------
// glinfo_cache_compare
//----------------------------------------------------------------------------------------------------------------------
static int glinfo_cache_compare(const void *elem0, const void *elem1)
{
    const glinfo_cache_t *gc0 = (const glinfo_cache_t *)elem0;
    const glinfo_cache_t *gc1 = (const glinfo_cache_t *)elem1;

    if (gc0->dpy == gc1->dpy)
        return (intptr_t)gc0->drawable - (intptr_t)gc1->drawable;
    return (intptr_t)gc0->dpy - (intptr_t)gc1->dpy;
}

//----------------------------------------------------------------------------------------------------------------------
// get_glinfo
//----------------------------------------------------------------------------------------------------------------------
static glinfo_cache_t *get_glinfo(Display *dpy, GLXDrawable drawable)
{
    static size_t s_glinfo_cache_count = 0;
    static glinfo_cache_t *s_glinfo_cache = NULL;

    glinfo_cache_t key;

    SDL_memset(&key, 0, sizeof(key));
    key.dpy = dpy;
    key.drawable = drawable;

    if (s_glinfo_cache_count)
    {
        glinfo_cache_t *glinfo_entry = bsearch(&key, s_glinfo_cache, s_glinfo_cache_count, sizeof(glinfo_cache_t), glinfo_cache_compare);
        if (glinfo_entry)
            return glinfo_entry;
    }

    void *data = realloc(s_glinfo_cache, (s_glinfo_cache_count + 1) * sizeof(glinfo_cache_t));
    if (data)
    {
        s_glinfo_cache = (glinfo_cache_t *)data;

        s_glinfo_cache[s_glinfo_cache_count++] = key;
        qsort(s_glinfo_cache, s_glinfo_cache_count, sizeof(glinfo_cache_t), glinfo_cache_compare);
        return &s_glinfo_cache[s_glinfo_cache_count - 1];
    }

    return NULL;
}

//----------------------------------------------------------------------------------------------------------------------
// read_proc_file
//----------------------------------------------------------------------------------------------------------------------
static char *read_proc_file(const char *filename)
{
    uint file_length = 0;
    int nbytes = 1024;
    char *filedata = NULL;

    int fd = open(filename, O_RDONLY);
    if (fd != -1)
    {
        for (;;)
        {
            // Make sure we've got enough room to read in another nbytes.
            char *data = (char *)realloc(filedata, file_length + nbytes);
            if (!data)
            {
                free(filedata);
                return NULL;
            }
            filedata = data;

            // Try to read in nbytes. read returns 0:end of file, -1:error.
            ssize_t length = read(fd, filedata + file_length, nbytes);
            if (length < 0)
                break;

            file_length += length;
            if (length != nbytes)
                break;
        }

        close(fd);
    }

    // Trim trailing whitespace.
    while ((file_length > 0) && SDL_isspace(filedata[file_length - 1]))
        file_length--;

    filedata[file_length] = 0;
    return filedata;
}

//----------------------------------------------------------------------------------------------------------------------
// vogl_is_debugger_present
//----------------------------------------------------------------------------------------------------------------------
static int vogl_is_debugger_present()
{
    int debugger_present = 0;
    char *status = read_proc_file("/proc/self/status");

    if (status)
    {
        static const char TracerPid[] = "TracerPid:";
        char *tracer_pid = strstr(status, TracerPid);

        if (tracer_pid)
            debugger_present = !!atoi(tracer_pid + sizeof(TracerPid) - 1);

        free(status);
    }

    return debugger_present;
}

//----------------------------------------------------------------------------------------------------------------------
// vogl_kbhit
//  See http://www.flipcode.com/archives/_kbhit_for_Linux.shtml
//----------------------------------------------------------------------------------------------------------------------
static int vogl_kbhit()
{
    static const int STDIN = 0;
    static int initialized = 0;

    if (!initialized)
    {
        // Use termios to turn off line buffering
        struct termios term;

        tcgetattr(STDIN, &term);
        term.c_lflag &= ~ICANON;
        tcsetattr(STDIN, TCSANOW, &term);
        setbuf(stdin, NULL);
        initialized = 1;
    }

    int bytesWaiting;
    ioctl(STDIN, FIONREAD, &bytesWaiting);
    return bytesWaiting;
}

//----------------------------------------------------------------------------------------------------------------------
// voglperf_init
//----------------------------------------------------------------------------------------------------------------------
static void voglperf_init()
{
    static int s_inited = 0;

    if (!s_inited)
    {
        s_inited = 1;

        // LOG_INFO, LOG_WARNING, LOG_ERR
        openlog(NULL, LOG_CONS | LOG_PERROR | LOG_PID, LOG_USER);

        char *cmd_line = SDL_getenv("VOGLPERF_CMD_LINE");
        if (cmd_line)
        {
            syslog(LOG_INFO, "(voglperf) built %s %s, begin initialization in %s\n", __DATE__, __TIME__, program_invocation_short_name);
            syslog(LOG_INFO, "(voglperf) VOGLPERF_CMD_LINE: '%s'\n", cmd_line);

            static const char s_msqid_arg[] = "--msqid=";
            const char *msqid_str = SDL_strstr(cmd_line, s_msqid_arg);
            if (msqid_str)
            {
                int msqid = atoi(msqid_str + sizeof(s_msqid_arg) - 1);
                if (msqid >= 0)
                {
                    struct mbuf_pid_t mbuf;

                    mbuf.mtype = 1;
                    mbuf.pid = getpid();

					int ret = msgsnd(msqid, &mbuf, sizeof(mbuf) - sizeof(mbuf.mtype), IPC_NOWAIT);
                    if (ret == 0)
                        g_msqid = msqid;

                    syslog(LOG_INFO, "(voglperf) msgsnd pid returns %d (msqid: %d)\n", ret, g_msqid);
                }
            }

            g_showfps = !!SDL_strstr(cmd_line, "--showfps");
            g_verbose = !!SDL_strstr(cmd_line, "--verbose");

            int debugger_pause = !!SDL_strstr(cmd_line, "--debugger-pause");
            if (debugger_pause && isatty(fileno(stdout)))
            {
                int sleeptime = 60000;
                int debugger_connected = 0;

                syslog(LOG_INFO, "(voglperf) Pausing %d ms or until debugger is attached (pid %d).\n", sleeptime, getpid());
                syslog(LOG_INFO, "(voglperf)   Or press any key to continue.\n");

                while (sleeptime >= 0)
                {
                    SDL_Delay(200);
                    sleeptime -= 200;
                    debugger_connected = vogl_is_debugger_present();
                    if (debugger_connected || vogl_kbhit())
                        break;
                }

                if (debugger_connected)
                {
                    syslog(LOG_INFO, "(voglperf)   Debugger connected...\n");
                }
            }

            static const char s_logfile_arg[] = "--logfile=";
            const char *logfile = SDL_strstr(cmd_line, s_logfile_arg);
            if (logfile)
            {
                char logfile_name[PATH_MAX];

                logfile += sizeof(s_logfile_arg) - 1;

                char delim_char = logfile[0];

                if (delim_char == '"' || delim_char == '\'')
                    logfile++;
                else
                    delim_char = ' ';

                const char *logfile_end = logfile;
                while (*logfile_end && (*logfile_end != delim_char))
                    logfile_end++;

                SDL_snprintf(logfile_name, sizeof(logfile_name), "%.*s", (int)(logfile_end - logfile), logfile);
                syslog(LOG_INFO, "(voglperf)  Framerate logfile: '%s'\n", logfile_name);

                g_logfile_fd = open(logfile_name, O_WRONLY | O_CREAT, 0666);
                if (g_logfile_fd == -1)
                {
                    syslog(LOG_ERR, "(voglperf) Error opening '%s': %s\n", logfile_name, strerror(errno));
                }
                else
                {
                    time_t now;
                    struct tm now_tm;
                    char timebuf[256];

                    time(&now);
                    strftime(timebuf, sizeof(timebuf), "%h %e %T", localtime_r(&now, &now_tm));

                    if (g_logfile_fd != -1)
                    {
                        SDL_snprintf(g_logfile_buf, sizeof(g_logfile_buf),
                                     "# %s - %s\n",
                                     timebuf, program_invocation_short_name);
                        HANDLE_EINTR(write(g_logfile_fd, g_logfile_buf, strlen(g_logfile_buf)));
                        g_logfile_buf_len = 0;
                    }

                    atexit(vogl_perf_destructor_func);
                }
            }

            if (g_showfps)
            {
                void *handle = SDL_LoadObject("libX11.so.6");
                if (handle)
                {
#define LOADX11FUNC(_func)                              \
    do                                                  \
    {                                                   \
        X11_##_func = SDL_LoadFunction(handle, #_func); \
    } while (0)
                    LOADX11FUNC(XLoadQueryFont);
                    LOADX11FUNC(XCreateGC);
                    LOADX11FUNC(XDrawString);
#undef LOADX11FUNC
                }

                if (!X11_XLoadQueryFont || !X11_XCreateGC || !X11_XDrawString)
                {
                    syslog(LOG_WARNING, "(voglperf) WARNING: Failed to load X11 function pointers.\n");
                    g_showfps = 0;
                }
            }
        }

        syslog(LOG_INFO, "(voglperf) end initialization\n");
    }
}

//----------------------------------------------------------------------------------------------------------------------
// dlopen interceptor
//----------------------------------------------------------------------------------------------------------------------
VOGL_API_EXPORT void *dlopen(const char *pFile, int mode)
{
    static dlopen_func_ptr_t s_pActual_dlopen;

    if (!s_pActual_dlopen)
    {
        s_pActual_dlopen = (dlopen_func_ptr_t)dlsym(RTLD_NEXT, "dlopen");
        if (!s_pActual_dlopen)
            return NULL;
    }

    // WARNING. Be careful in here. Some libraries are open'd very early before the voglcore library heap
    //  has been initialized...

    if (g_verbose)
    {
        syslog(LOG_INFO, "(voglperf) %s %s %i\n", __PRETTY_FUNCTION__, pFile ? pFile : "(nullptr)", mode);
    }

    return (*s_pActual_dlopen)(pFile, mode);
}

//----------------------------------------------------------------------------------------------------------------------
// dlopen glXMakeCurrent
//$ TODO: Need to hook glXMakeCurrentReadSGI_func_ptr_t
//----------------------------------------------------------------------------------------------------------------------
VOGL_API_EXPORT Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx)
{
    static glXMakeCurrent_func_ptr_t s_pActual_glXMakeCurrent;

    if (!s_pActual_glXMakeCurrent)
    {
        s_pActual_glXMakeCurrent = (glXMakeCurrent_func_ptr_t)dlsym(RTLD_NEXT, "glXMakeCurrent");
        if (!s_pActual_glXMakeCurrent)
            return False;
    }

    voglperf_init();

    if (g_verbose)
    {
        syslog(LOG_INFO, "(voglperf) %s %p %lu %p\n", __PRETTY_FUNCTION__, dpy, drawable, ctx);
    }

    Bool ret = (*s_pActual_glXMakeCurrent)(dpy, drawable, ctx);
    if (!ret)
        return ret;

    glinfo_cache_t *glinfo = get_glinfo(dpy, drawable);
    if (glinfo)
    {
        static glGetString_func_ptr_t s_pActual_glGetString;

        if (!s_pActual_glGetString)
            s_pActual_glGetString = (glGetString_func_ptr_t)dlsym(RTLD_NEXT, "glGetString");
        if (s_pActual_glGetString)
        {
            if (glinfo->ctx != ctx)
            {
                // Set new ctx and clear strings so they'll be reloaded.
                glinfo->ctx = ctx;
                glinfo->glstrings_valid = 0;
            }

            if (!glinfo->glstrings_valid)
            {
                static const GLubyte s_nil[] = { 0 };

                glinfo->glstrings_valid = 1;

                glinfo->renderer = s_pActual_glGetString(GL_RENDERER);
                glinfo->vendor = s_pActual_glGetString(GL_VENDOR);
                glinfo->version = s_pActual_glGetString(GL_VERSION);

                if (!glinfo->renderer)
                    glinfo->renderer = s_nil;
                if (!glinfo->vendor)
                    glinfo->vendor = s_nil;
                if (!glinfo->version)
                    glinfo->version = s_nil;

                syslog(LOG_INFO, "(voglperf) glinfo: '%s' '%s' '%s'\n", glinfo->vendor, glinfo->renderer, glinfo->version);
            }
        }
    }

    return ret;
}

//----------------------------------------------------------------------------------------------------------------------
// voglperf_swap_buffers
//----------------------------------------------------------------------------------------------------------------------
static void voglperf_swap_buffers(Display *dpy, GLXDrawable drawable, int flush_logfile)
{
    typedef struct frameinfo_t
    {
        uint64_t time_benchmark;
        uint64_t time_last_frame;
        uint64_t frame_min;
        uint64_t frame_max;
        unsigned int frame_count;
        char text[256];
    } frameinfo_t;
    static frameinfo_t s_frameinfo = { 0, 0, (uint64_t)-1, 0, 0, { 0 } };
    static const uint64_t g_BILLION = 1000000000;
    static const double g_rcpMILLION = (1.0 / 1000000);

    // Get current time.
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);

    uint64_t time_cur = (time.tv_sec * g_BILLION) + time.tv_nsec;

    if (s_frameinfo.time_last_frame)
    {
        uint64_t time_frame = time_cur - s_frameinfo.time_last_frame;

        s_frameinfo.frame_count++;
        s_frameinfo.time_benchmark += time_frame;

        if (g_logfile_fd != -1)
        {
            SDL_snprintf(g_logfile_buf + g_logfile_buf_len, sizeof(g_logfile_buf) - g_logfile_buf_len, "%.2f\n", time_frame * g_rcpMILLION);
            g_logfile_buf_len += strlen(g_logfile_buf + g_logfile_buf_len);
        }

        if (s_frameinfo.frame_min > time_frame)
            s_frameinfo.frame_min = time_frame;
        if (s_frameinfo.frame_max < time_frame)
            s_frameinfo.frame_max = time_frame;

        // Check if one second has gone by.
        if ((s_frameinfo.time_benchmark >= g_BILLION) || flush_logfile)
        {
            struct mbuf_fps_t mbuf;

            mbuf.mtype = MSGTYPE_FPS;
            mbuf.fps = s_frameinfo.frame_count * (double)g_BILLION / s_frameinfo.time_benchmark;
            mbuf.frame_count = s_frameinfo.frame_count;
            mbuf.frame_time = s_frameinfo.time_benchmark * g_rcpMILLION;
            mbuf.frame_min =s_frameinfo.frame_min * g_rcpMILLION;
            mbuf.frame_max = s_frameinfo.frame_max * g_rcpMILLION;

            SDL_snprintf(s_frameinfo.text, sizeof(s_frameinfo.text),
                         "%.2f fps frames:%u time:%.2fms min:%.2fms max:%.2fms",
                         mbuf.fps, mbuf.frame_count, mbuf.frame_time, mbuf.frame_min, mbuf.frame_max);
            syslog(LOG_INFO, "(voglperf) %s\n", s_frameinfo.text);

            if (g_msqid >= 0)
            {
                int ret = msgsnd(g_msqid, &mbuf, sizeof(mbuf) - sizeof(mbuf.mtype), IPC_NOWAIT);
                if (ret == -1)
                {
                    syslog(LOG_ERR, "(voglperf) msgsnd fps failed: %d. %s\n", ret, strerror(errno));
                    g_msqid = -1;
                }
            }

            if (g_logfile_fd != -1)
            {
                char buf[256];

                snprintf(buf, sizeof(buf), "# %s\n", s_frameinfo.text);
                HANDLE_EINTR(write(g_logfile_fd, buf, strlen(buf)));

                HANDLE_EINTR(write(g_logfile_fd, g_logfile_buf, g_logfile_buf_len));
                g_logfile_buf_len = 0;
            }

            // Reset for next benchmark run.
            s_frameinfo.time_benchmark = 0;
            s_frameinfo.frame_min = (uint64_t)-1;
            s_frameinfo.frame_max = 0;
            s_frameinfo.frame_count = 0;
        }
    }

    s_frameinfo.time_last_frame = time_cur;

    if (g_showfps && dpy && drawable)
    {
        glinfo_cache_t *glinfo = get_glinfo(dpy, drawable);

        if (glinfo)
        {
            if (!glinfo->gc)
            {
                XGCValues ctx_vals;
                unsigned long gcflags = GCForeground | GCBackground;

                // static const char g_MessageBoxFontLatin1[] = "-*-*-medium-r-normal--0-120-*-*-p-0-iso8859-1";
                // XFontStruct *font_struct = X11_XLoadQueryFont(dpy, g_MessageBoxFontLatin1);
                // if (font_struct)
                // {
                //     gcflags |= GCFont;
                //     ctx_vals.font = font_struct->fid;
                // }

                ctx_vals.foreground = 0xff0000;
                ctx_vals.background = 0x000000;

                //$ TODO: Free these?
                //   X11_XFreeGC(dpy, gc);
                //   X11_XFreeFont( dpy, font_struct );
                glinfo->gc = X11_XCreateGC(dpy, drawable, gcflags, &ctx_vals);
            }

            if (glinfo->gc)
            {
                // This will flash as we're adding it after the present.
                // Might also not work on some drivers as they don't sync between X11 and GL.
                X11_XDrawString(dpy, drawable, glinfo->gc, 10, 20, s_frameinfo.text, (int)strlen(s_frameinfo.text));
            }
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// glXSwapBuffers interceptor
//----------------------------------------------------------------------------------------------------------------------
VOGL_API_EXPORT void glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
    static glXSwapBuffers_func_ptr_t s_pActual_glXSwapBuffers;

    if (!s_pActual_glXSwapBuffers)
    {
        s_pActual_glXSwapBuffers = (glXSwapBuffers_func_ptr_t)dlsym(RTLD_NEXT, "glXSwapBuffers");
        if (!s_pActual_glXSwapBuffers)
            return;
    }

    voglperf_init();

    if (g_verbose)
    {
        syslog(LOG_INFO, "(voglperf) %s %p %lu\n", __PRETTY_FUNCTION__, dpy, drawable);
    }

    // Call real glxSwapBuffers function.
    (*s_pActual_glXSwapBuffers)(dpy, drawable);

    voglperf_swap_buffers(dpy, drawable, 0);
}

//----------------------------------------------------------------------------------------------------------------------
// vogl_perf_destructor_func
//----------------------------------------------------------------------------------------------------------------------
__attribute__((destructor)) static void vogl_perf_destructor_func()
{
    if (g_logfile_fd != -1)
    {
        // Flush whatever framerate numbers we've built up.
        voglperf_swap_buffers(NULL, None, 1);

        if (g_msqid != -1)
        {
            struct mbuf_fps_t mbuf;

            // Let voglperfrun know we're exiting.
            mbuf.mtype = MSGTYPE_FPS;
            mbuf.frame_count = (uint32_t)-1;

			int ret = msgsnd(g_msqid, &mbuf, sizeof(mbuf) - sizeof(mbuf.mtype), IPC_NOWAIT);
            if (ret == -1)
                syslog(LOG_ERR, "(voglperf) msgsnd failed: %d. %s\n", ret, strerror(errno));

            g_msqid = -1;
        }

        close(g_logfile_fd);
        g_logfile_fd = -1;
    }
}
