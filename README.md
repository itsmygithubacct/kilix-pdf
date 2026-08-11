# kilix-pdf

`kilix-pdf` is a PDF viewer designed for a GPU terminal. It renders PDF pages
with Poppler and Cairo, keeps the rendered page in memory while you scroll,
and sends retained-image scroll operations to Kilix Kitty instead of encoding
the whole viewport again.

The executable name is `kilix-pdf-viewer`; `kilix-pdf` remains the separate
PDF-to-Markdown converter. The launcher tries the native terminal viewer first
and opens the same file in Evince if native dependencies, terminal graphics,
or encrypted-document UI are unavailable. Launching without a path opens
Evince's file chooser.

## Build and test

Clone recursively, then build with the Poppler GLib, Cairo, and GLib
development packages installed:

```sh
git clone --recurse-submodules \
  https://github.com/itsmygithubacct/kilix-pdf.git
cd kilix-pdf
make native
make test
make test-native
make sanitize
```

Plain `make` is intentionally usable without development headers: it leaves
the tracked Evince launcher available. `make native` is the strict release
check and fails when the CPU renderer cannot be built.

Headless rasterization is available for validation:

```sh
build/kilix-pdf-core --render-page document.pdf page.png
build/kilix-pdf-core --benchmark document.pdf
```

## Controls

- `j`, `k`, Up, Down, or the mouse wheel scroll the rendered page.
- Page Up/Down, Left/Right, `p`, `n`, or Space change pages.
- `+` and `-` zoom; `0` fits the page and `w` fits its width.
- Home/End jump to the first/last page. `q` or Escape exits.

## Rendering and fallbacks

Poppler interprets PDF content and Cairo rasterizes the current page on the
CPU. Scrolling reuses that page surface. Kilix Kitty can compose the retained
content viewport overlap on a hardware GPU while leaving the toolbar fixed;
when no usable GPU exists, the terminal's validated CPU upload path produces
the same result. On an unmodified Kitty-compatible terminal, the framebuffer
library automatically sends a full frame instead of the Kilix extension.
Framebuffer images are layered below terminal foreground UI so Kilix's
software mouse pointer remains visible over the document.

The launcher treats exit status 69 as “native viewer unavailable” and then
executes `/usr/bin/evince`. Other errors are preserved so a corrupt path or PDF
does not silently open a second application. Pass `--no-fallback` first to
disable the Evince handoff. Tests may override `KILIX_PDF_VIEWER_CORE` and
`KILIX_PDF_VIEWER_EVINCE`; normal users do not need either variable.

## Scope

This first release deliberately keeps PDF interpretation and rasterization on
the mature Poppler/Cairo path. The terminal protocol and renderer boundary are
separate, so an experimental GPU rasterizer can be measured page by page and
fall back to this CPU implementation whenever it encounters an unsupported
operator or device.

## License

GPL-3.0-or-later. See `LICENSE` and `THIRD-PARTY-NOTICES.md`.
