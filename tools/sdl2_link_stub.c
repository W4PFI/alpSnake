/*
 * sdl2_link_stub.c - a fake libSDL2 used ONLY at link time, never shipped.
 *
 * Both programs we build call into SDL2, which already exists on the cabinet
 * (as libSDL2-2.0.so.0). The core uses just SDL_PushEvent (its idle
 * keepalive); the backglass helper uses SDL for its whole window and drawing.
 * To link them on a Mac we would normally need a real aarch64 Linux build of
 * SDL2. We don't: a linker only needs to know that a shared library with the
 * right soname exports the right symbol names. It does not care what the
 * functions do or what arguments they take.
 *
 * So tools/build.py compiles this file with
 *     clang --target=aarch64-linux-gnu -shared -fPIC -nostdlib
 *           -Wl,-soname,libSDL2-2.0.so.0  ->  build/libSDL2-link-only.so
 * and passes that .so on the link lines. Our binaries then record
 * "NEEDED libSDL2-2.0.so.0", and on the cabinet the dynamic loader resolves
 * those names against the real SDL2 by that soname. These empty bodies never
 * run. (Set SDL2_LINK_LIBRARY to link against a real SDL2 instead.)
 *
 * Every SDL function either program calls must be listed here, otherwise the
 * -Wl,--no-undefined link fails, which is exactly what we want: a missing name
 * is caught at build time rather than on the cabinet.
 */

/* Defines an exported, empty function with the given SDL name. The signature
   is deliberately wrong (void(void)); only the symbol name matters. */
#define SDL2_LINK_SYMBOL(name) void name(void) {}

SDL2_LINK_SYMBOL(SDL_CreateRenderer)
SDL2_LINK_SYMBOL(SDL_CreateTexture)
SDL2_LINK_SYMBOL(SDL_CreateTextureFromSurface)
SDL2_LINK_SYMBOL(SDL_CreateWindow)
SDL2_LINK_SYMBOL(SDL_Delay)
SDL2_LINK_SYMBOL(SDL_DestroyRenderer)
SDL2_LINK_SYMBOL(SDL_DestroyTexture)
SDL2_LINK_SYMBOL(SDL_DestroyWindow)
SDL2_LINK_SYMBOL(SDL_FreeSurface)
SDL2_LINK_SYMBOL(SDL_Init)
SDL2_LINK_SYMBOL(SDL_LoadBMP_RW)
SDL2_LINK_SYMBOL(SDL_PushEvent)
SDL2_LINK_SYMBOL(SDL_Quit)
SDL2_LINK_SYMBOL(SDL_RWFromFile)
SDL2_LINK_SYMBOL(SDL_RenderCopy)
SDL2_LINK_SYMBOL(SDL_RenderFillRect)
SDL2_LINK_SYMBOL(SDL_RenderPresent)
SDL2_LINK_SYMBOL(SDL_SetRenderDrawColor)
SDL2_LINK_SYMBOL(SDL_SetTextureBlendMode)
SDL2_LINK_SYMBOL(SDL_SetTextureColorMod)
SDL2_LINK_SYMBOL(SDL_UpdateTexture)
