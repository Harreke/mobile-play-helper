/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MPH_QUALITY_H
#define MPH_QUALITY_H
struct scrcpy_options;
struct sc_input_manager;
void mph_quality_load(struct scrcpy_options *opts);
void mph_quality_dialog(struct sc_input_manager *im);
int mph_quality_finish(int code);
void mph_quality_connected(void);
#endif
