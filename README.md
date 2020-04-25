# Nintendo 64 Fast3D renderer

Implementation of a Fast3D renderer for games built originally for the Nintendo 64 platform.

For rendering OpenGL and Direct3D 12 are supported.

Supported windowing systems are GLX (used on Linux), DXGI (used on Windows) and SDL (generic).

# Usage

See `gfx_pc.h`. You will also need a copy of `PR/gbi.h`, found in libultra.

First call `gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi)` and supply the desired backends at program start.

Each game main loop iteration should look like this:

```C
gfx_start_frame(); // Handles input events such as keyboard and window events
// perform game logic here
gfx_run(cmds); // submit display list and render a frame
// do more expensive work here, such as play audio
gfx_end_frame(); // this just waits until the frame is shown on the screen (vsync), to provide correct game timing
```

For the best experience, please change the Vtx and Mtx structures to use floats instead of fixed point arithmetic.

# License

See LICENSE.txt. Redistributions are allowed only in source form, not in binary form.
