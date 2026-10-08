# Headless Test Execution Rule

- Any test or program execution that would pop up a graphical window or initialize audio/display
  subsystems (such as OpenGL, Vulkan, SDL, etc.) MUST be run in headless mode unless a visual
  inspection / visual confirmation is specifically asked for by the user.

- For SDL / OpenGL / Vulkan based applications and tests, use the appropriate headless environment
  variables, with the XDG directories pointed into `build/xdg/` so the run cannot write to your real
  permascroll, settings or cache, for example:

  ```bash
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
    XDG_DATA_HOME=$PWD/build/xdg/data XDG_CONFIG_HOME=$PWD/build/xdg/config \
    XDG_CACHE_HOME=$PWD/build/xdg/cache <command>
  ```

- Always ensure headless flags/environment variables are set when running automated test suites
  (`gleditor_test`, `xudu_test`, etc.).
