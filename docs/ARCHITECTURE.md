# Architecture

The interactive path has three deliberately independent layers:

1. Poppler parses a page and Cairo rasterizes it once at the selected zoom.
2. The viewer composites a viewport and fixed toolbar into an RGBA frame.
3. `kitty-terminal-session` presents the frame and decodes ordered input.

Page changes, zooms, and resizes send a full frame. A small vertical pan sends
the complete new frame plus its scroll delta to `kittyts_present_scroll()`.
The framebuffer library asks Kilix Kitty to move the retained overlap and
patches only exposed strips and fixed chrome. Capability, state, and cost
checks revert to a full frame automatically.

This boundary is also the GPU-rasterization seam. A future renderer can produce
the cached page surface through another backend, but every page must retain the
Poppler/Cairo implementation as a correctness fallback. No Poppler fork is
needed until a measured renderer prototype demonstrates a generally useful
upstreamable change.
