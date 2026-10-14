/* Modified for MobilePlayHelper; changes from the upstream scrcpy version. */
#ifndef SCRCPY_H
#define SCRCPY_H

#include "common.h"

#include "options.h"

enum scrcpy_exit_code {
    // Normal program termination
    SCRCPY_EXIT_SUCCESS,

    // No connection could be established
    SCRCPY_EXIT_FAILURE,

    // Device was disconnected while running
    SCRCPY_EXIT_DISCONNECTED,
    SCRCPY_EXIT_MPH_RESTART,
    SCRCPY_EXIT_MPH_BENCHMARK,
};

enum scrcpy_exit_code
scrcpy(struct scrcpy_options *options);

#endif
