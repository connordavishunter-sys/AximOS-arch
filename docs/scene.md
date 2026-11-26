# wm on SceneFX: GPU compositing

wm draws the whole desktop on the CPU into one image, copying every app window's
pixels into it, and uploads that to the GPU each frame. At 2560x1600 that's too much
work for smooth video. This moves wm onto SceneFX, wlroots' scene graph with blur,
shadows and rounded corners on the GPU, in stages, keeping wm working after each.

## The scene

From the bottom:

1. **Desktop**: wallpaper, icons, the hint and renaming boxes, drawn by wm into one
   buffer the size of the screen. It changes rarely.
2. **Windows**, in wm's stacking order, each a tree of:
   - a SceneFX shadow, the frame's size, with its corner radius;
   - a SceneFX blur behind the title bar;
   - the frame: title bar and border, drawn by wm into a buffer of its own,
     translucent where the title bar's glass is;
   - the app's surfaces, straight from its buffers, as `wlr_scene_subsurface_tree`,
     with rounded bottom corners, or a popup's surfaces, with no frame;
   - while it opens, is restored, maximized or tiled, a copy of it, which stands in
     for the rest (see Animations).
3. **Overlay**: each part wm draws over the windows, in a buffer of its own: the
   taskbar, the open panel, menus, notices, the volume and brightness popup, the
   switcher, the overview, the password dialog, snapping's outline and the dragged
   file's icon, over closing and minimized windows' copies. Each glass part has a
   SceneFX blur behind it.
4. **Cursor**: wm's own, in a small buffer.

## Why many buffers

SceneFX drops a buffer's texture whenever its buffer changes, and uploads all of it
again, so one full-screen overlay would be uploaded whole each time the cursor moved.
Each part in its own small buffer is uploaded only when it changes.

## Drawing into them

wm's drawing stays as it is: `add_damage` says what changed, and `compose` draws it.
For each damaged rectangle, each part it overlaps is drawn in that rectangle, into a
view of the part's own buffer, by the part's existing `draw_*` function with that
view's origin as the offset. Parts other than the desktop start from transparent.

The desktop is the one big part, so it's drawn again only where it changed itself,
its icons or wallpaper, rather than wherever anything changed.

Transparency: wm's pixels' top byte is how see-through they are, 0 for solid.
`gui_mix` blends that byte as well as the colour, so text and rounded edges drawn on
a translucent glass tint come out translucent where they should, and solid canvases,
whose top byte is 0, are unchanged. Glass is a tint at the bar's alpha, with the
GPU's blur behind it, rather than wm's CPU blur of what's beneath. Buffers are
converted to premultiplied ARGB for the GPU as they're drawn.

## Window pixels

With a renderer, wlroots uploads app buffers into textures and releases them, so wm
no longer has a copy of each window. Where it still needs one, for opening and
closing animations, the overview and the taskbar's previews, it reads the window's
textures back from the GPU when that starts.

## Animations

SceneFX can't scale a tree, so a moving window is shown by a copy of it: a tree of
its own, with a buffer node for each of the window's, its frame part's and its
surfaces', showing the same buffers. The GPU draws them from the textures it already
has, as an app's buffer's texture is its `wlr_client_buffer`'s, so nothing's read
back or uploaded. Each frame, wm says where the frame is now and how solid, and each
node is placed and sized in proportion, with a shadow, border and blur of the copy's
own around them. A window opening or moving is copied again each frame, which only
points nodes at buffers, so it shows what the app draws meanwhile; a closing or
minimized one keeps its copy, which holds its buffers after the app's let them go.
A moving window's copy is let go of when it gets there, so the app has its buffers
back to draw in.

## Fullscreen

A fullscreen window, as the app asks, through xdg-shell or EWMH, or Super+F, has no
frame and covers the screen, over the taskbar. Once it's on top, if it's solid,
nothing under it is in the scene: the desktop and the other windows are disabled, so the GPU composites
that window and what's over the windows, such as a notice, and nothing else.

## Stages

1. **Scene, windows on the GPU** (fixes video stutter). Done: the renderer, the scene
   and its layers, app windows as subsurface trees with rounded corners, title bars,
   borders, shadows and blurs as nodes, each part over the windows in its own buffer,
   flyouts faded and slid by the GPU, and the night light as a colour transform. In a
   VM at 2x, playing video, wm's own thread went from doing all the copying to about
   10% of a core. Opening and closing animations still draw their copies on the CPU,
   in a part, as they did.
2. **Animations on the GPU**. Done: windows opening, closing, minimizing, restoring,
   maximizing and tiling are copies the GPU moves, scales and fades, sharing the
   window's textures, rather than drawn on the CPU each frame. Fullscreen windows
   are all that's composited, but for what's over them.
3. **Direct scanout**: a fullscreen app's buffer goes straight to the screen, with
   no compositing at all, where the GPU's planes can take it.
