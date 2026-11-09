# kilix-pdf

`kilix-pdf` is a PDF viewer designed for a GPU terminal. It renders PDF pages
with Poppler and Cairo, keeps the rendered page in memory while you scroll,
and sends retained-image scroll operations to Kilix Kitty instead of encoding
the whole viewport again.

On Plebian-OS, `pleb install` builds it at first boot
(`kilix pdf-view --install-only`) from the commit pinned in the Kilix content
catalog as `kilix-pdf`. Elsewhere, `kilix install kilix-pdf` installs it, and
`kilix pdf-view PDF` installs it on first use and opens the file in the current
pane.

The executable name is `kilix-pdf-viewer`; `kilix-pdf` remains the separate
PDF-to-Markdown converter. The launcher tries the native terminal viewer first
and opens the same file in Evince if native dependencies, terminal graphics,
or encrypted-document UI are unavailable. Launching without a path opens
Evince's file chooser.

![`kilix pdf-view` showing a one-page test document in a pane](docs/images/viewer.png)

## What it does

- Opens one PDF in the terminal: page navigation, zoom, fit page, fit width,
  mouse-wheel scrolling and resize handling.
- Rasterizes each page once on the CPU with Poppler and Cairo and reuses that
  surface while you scroll.
- Sends small vertical scrolls as retained-image moves, so Kilix Kitty patches
  only the exposed strips; other terminals receive full frames.
- Falls back to Evince when the native viewer cannot run, and keeps every
  other error visible.
- Renders a page to PNG or times rendering without a terminal, for tests.

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

`make install` copies `kilix-pdf-viewer`, and `build/kilix-pdf-core` when it
was built, to `$(PREFIX)/bin` (default `/usr/local`). The content catalog runs
`make all` and launches `kilix-pdf-viewer` from the checkout.

Headless rasterization is available for validation:

```sh
build/kilix-pdf-core --render-page document.pdf page.png
build/kilix-pdf-core --benchmark document.pdf
```

`build/kilix-pdf-core --probe` prints the viewer, Poppler and Cairo versions
and the rasterizer and composition paths.

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

```
PDF --> Poppler + Cairo --> page surface --> viewport + toolbar --> kitty-terminal-session
        (CPU, once per       (kept while       (RGBA frame)          full frame, or
         page and zoom)       you scroll)                             retained scroll + strips
```

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

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): the three layers, retained
  scrolling and the renderer seam.
- [`CHANGELOG.md`](CHANGELOG.md): release history.
- [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md): the linked system
  libraries, the bundled terminal libraries and the Evince fallback.

## Related repositories

- [kitty-terminal-session](https://github.com/itsmygithubacct/kitty-terminal-session#readme)
  (submodule): frame presentation and ordered input, built on
  [kitty-framebuffer](https://github.com/itsmygithubacct/kitty-framebuffer#readme)
  and [kitty-input](https://github.com/itsmygithubacct/kitty-input#readme).
- [kitty](https://github.com/itsmygithubacct/kitty#readme): the Kilix Kitty
  fork whose retained-scroll extension this viewer uses.
- [kilix-pdf-conversion](https://github.com/itsmygithubacct/kilix-pdf-conversion#readme):
  the separate PDF-to-Markdown converter (`kilix-pdf`).
- [kilix-content](https://github.com/itsmygithubacct/kilix-content#readme) and
  [kilix](https://github.com/itsmygithubacct/kilix#readme): the catalog entry
  and the `kilix pdf-view` verb.

## Part of Plebian-OS

kilix-pdf is one of the repositories behind
[Plebian-OS](https://github.com/itsmygithubacct/plebian-os#readme), a Debian
install whose desktop is [Kilix](https://github.com/itsmygithubacct/kilix#readme)
running the [Kilix 95](https://github.com/itsmygithubacct/kilix-95#readme)
desktop through [Pleb](https://github.com/itsmygithubacct/pleb#readme).
`pleb install` builds it at first boot from the commit pinned in the Kilix
content catalog; it is not carried in the installer image. The map of every repository and how it ships is on the
[components page](https://github.com/itsmygithubacct/plebian-os/blob/main/docs/components.md).

## License

GPL-3.0-or-later. See `LICENSE` and `THIRD-PARTY-NOTICES.md`.
