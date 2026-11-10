/* SDL2 X11 backend draw/input probe (disable other SDL video backends in runner). */
#include <SDL.h>
#include <stdio.h>
#include <time.h>
static double seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1000000000.0;
}
int main(void) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "%s\n", SDL_GetError()); return 2; }
  SDL_Window *window = SDL_CreateWindow("SDL2 probe", 30, 30, 160, 100, SDL_WINDOW_SHOWN);
  if (!window) { fprintf(stderr, "%s\n", SDL_GetError()); SDL_Quit(); return 2; }
  SDL_Surface *surface = SDL_GetWindowSurface(window);
  if (!surface) { fprintf(stderr, "%s\n", SDL_GetError()); SDL_DestroyWindow(window); SDL_Quit(); return 2; }
  SDL_FillRect(surface, NULL, SDL_MapRGB(surface->format, 0, 0, 0));
  SDL_Rect r = {10, 10, 40, 30};
  SDL_FillRect(surface, &r, SDL_MapRGB(surface->format, 255, 0, 0));
  SDL_UpdateWindowSurface(window);
  int press = 0, release = 0;
  double until = seconds() + 4.0;
  while (seconds() < until && !release) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_MOUSEBUTTONDOWN) ++press;
      if (e.type == SDL_MOUSEBUTTONUP) ++release;
    }
    SDL_Delay(10);
  }
  printf("sdl2 driver=%s format=%s press=%d release=%d\n", SDL_GetCurrentVideoDriver(),
         SDL_GetPixelFormatName(surface->format->format), press, release);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return press && release ? 0 : 1;
}
