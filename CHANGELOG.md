# Changelog

## 0.1.0

- Add a Poppler/Cairo terminal-native PDF viewer with page caching, navigation,
  zoom, mouse-wheel input, resize handling, and headless PNG validation.
- Use retained-scroll terminal composition for small vertical moves, with
  automatic full-frame and CPU fallbacks.
- Clip retained scrolling to the document viewport so the toolbar cannot smear
  into the page, and keep the terminal software pointer above the framebuffer.
- Use protocol-valid multi-chunk frame-edit continuations for clean text-heavy
  scrolling in both directions.
- Keep the existing PDF conversion command distinct through the
  `kilix-pdf-viewer` executable name.
- Fall back to Evince when the native viewer or its interactive encrypted-PDF
  flow is unavailable.
- Open Evince's graphical file chooser for launcher invocations that do not
  provide a PDF path.
