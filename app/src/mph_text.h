/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MPH_TEXT_H
#define MPH_TEXT_H
#include <SDL3/SDL.h>
float mph_text_width(SDL_Renderer *renderer, const char *text, float height);
void mph_text_draw(SDL_Renderer *renderer, float x, float y, const char *text, float height);
void mph_text_destroy(SDL_Renderer *renderer);
#endif
