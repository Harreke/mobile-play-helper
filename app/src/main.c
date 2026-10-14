/* Modified for MobilePlayHelper; changes from the upstream scrcpy version. */
#include "common.h"

#include <stdbool.h>
#include <stdio.h>
#ifdef HAVE_V4L2
# include <libavdevice/avdevice.h>
#endif
#include <SDL3/SDL.h>

#include "cli.h"
#include "events.h"
#include "options.h"
#include "mph_quality.h"
#include "scrcpy.h"
#ifdef HAVE_USB
# include "usb/scrcpy_otg.h"
#endif
#include "util/log.h"
#include "util/net.h"
#include "util/term.h"
#include "version.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include "util/str.h"
#endif

static int
main_scrcpy(int argc, char *argv[]) {
#ifdef _WIN32
    bool gui = GetConsoleWindow() == NULL;
    if (gui) {
        wchar_t logpath[32768];
        DWORD length=GetModuleFileNameW(NULL,logpath,32768);
        if(length && length<32700) {
            wchar_t *slash=wcsrchr(logpath,L'\\');
            if(slash) {
                wcscpy(slash+1,L"MobilePlayHelper.log");
                wchar_t previous_log[32768];
                swprintf(previous_log,32768,L"%ls.previous",logpath);
                MoveFileExW(logpath,previous_log,MOVEFILE_REPLACE_EXISTING);
                FILE *log=_wfreopen(logpath,L"w",stderr);
                if(log) SetStdHandle(STD_ERROR_HANDLE,(HANDLE)_get_osfhandle(_fileno(stderr)));
                if (log && _wfreopen(L"NUL",L"w",stdout)) {
                    _dup2(_fileno(stderr),_fileno(stdout));
                    SetStdHandle(STD_OUTPUT_HANDLE,(HANDLE)_get_osfhandle(_fileno(stdout)));
                }
            }
        }
    }
    SYSTEMTIME now;GetLocalTime(&now);
    fprintf(stderr,"MPH session: %04u-%02u-%02u %02u:%02u:%02u, PID=%lu\n",
        now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,GetCurrentProcessId());
    // disable buffering, we want logs immediately
    // even line buffering (setvbuf() with mode _IOLBF) is not sufficient
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
#endif

    printf("MobilePlayHelper 0.3.0 (scrcpy " SCRCPY_VERSION ")"
           " <https://github.com/harreke/mobile-play-helper>\n");

    struct scrcpy_cli_args args = {
        .opts = scrcpy_options_default,
        .help = false,
        .version = false,
        .pause_on_exit = SC_PAUSE_ON_EXIT_UNDEFINED,
    };

#ifndef NDEBUG
    args.opts.log_level = SC_LOG_LEVEL_DEBUG;
#endif

    mph_quality_load(&args.opts);
    enum scrcpy_exit_code ret;

    bool term_title_saved = false;

    if (!scrcpy_parse_args(&args, argc, argv)) {
        ret = SCRCPY_EXIT_FAILURE;
        goto end;
    }

    if (!args.opts.window_title) args.opts.window_title="MobilePlayHelper";
    sc_set_log_level(args.opts.log_level);

    if (args.opts.update_terminal_title) {
        sc_term_save_title();
        sc_term_set_title("MobilePlayHelper");
        term_title_saved = true;
    }

    if (args.help) {
        scrcpy_print_usage(argv[0]);
        ret = SCRCPY_EXIT_SUCCESS;
        goto end;
    }

    if (args.version) {
        scrcpy_print_version();
        ret = SCRCPY_EXIT_SUCCESS;
        goto end;
    }

#ifdef SCRCPY_LAVF_REQUIRES_REGISTER_ALL
    av_register_all();
#endif

#ifdef HAVE_V4L2
    if (args.opts.v4l2_device) {
        avdevice_register_all();
    }
#endif

    if (!net_init()) {
        ret = SCRCPY_EXIT_FAILURE;
        goto end;
    }

    sc_log_configure();

    if (!sc_main_thread_init()) {
        ret = SCRCPY_EXIT_FAILURE;
        goto net_cleanup;
    }

#ifdef HAVE_USB
    ret = args.opts.otg ? scrcpy_otg(&args.opts) : scrcpy(&args.opts);
#else
    ret = scrcpy(&args.opts);
#endif

    sc_main_thread_destroy();

net_cleanup:
    net_cleanup();

end:
    if (
#ifdef _WIN32
        !gui &&
#endif
        (args.pause_on_exit == SC_PAUSE_ON_EXIT_TRUE ||
            (args.pause_on_exit == SC_PAUSE_ON_EXIT_IF_ERROR &&
                ret != SCRCPY_EXIT_SUCCESS))) {
        printf("Press Enter to continue...\n");
        getchar();
    }

    if (term_title_saved) {
        sc_term_set_title(""); // fallback if restore is ignored
        sc_term_restore_title();
    }

#ifdef _WIN32
    ret = mph_quality_finish(ret);
    LOGI("MPH session ended: exit_code=%d",ret);
    if(gui && !getenv("MPH_BENCHMARK") && ret != SCRCPY_EXIT_SUCCESS)
        MessageBoxW(NULL,L"无法连接手机，或设备已断开。请开启 USB 调试并授权这台电脑；连接多台设备时，请只保留目标手机。详细信息见 dist/MobilePlayHelper.log。",L"MobilePlayHelper",MB_OK|MB_ICONERROR);
#endif
    return ret;
}

int
main(int argc, char *argv[]) {
#ifndef _WIN32
    return main_scrcpy(argc, argv);
#else
    (void) argc;
    (void) argv;
    int wargc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        LOG_OOM();
        return SCRCPY_EXIT_FAILURE;
    }

    char **argv_utf8 = malloc((wargc + 1) * sizeof(*argv_utf8));
    if (!argv_utf8) {
        LOG_OOM();
        LocalFree(wargv);
        return SCRCPY_EXIT_FAILURE;
    }

    argv_utf8[wargc] = NULL;

    for (int i = 0; i < wargc; ++i) {
        argv_utf8[i] = sc_str_from_wchars(wargv[i]);
        if (!argv_utf8[i]) {
            LOG_OOM();
            for (int j = 0; j < i; ++j) {
                free(argv_utf8[j]);
            }
            LocalFree(wargv);
            free(argv_utf8);
            return SCRCPY_EXIT_FAILURE;
        }
    }

    LocalFree(wargv);

    int ret = main_scrcpy(wargc, argv_utf8);

    for (int i = 0; i < wargc; ++i) {
        free(argv_utf8[i]);
    }
    free(argv_utf8);

    return ret;
#endif
}
