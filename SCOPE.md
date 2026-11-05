# Project Vision & Scope: CrossDink

The goal of this CrossPoint fork is to provide useful enhancements while still adhering to the core principles of Crosspoint. If you have a major feature request, it should first be directed at the main project since this is a downstream project that consumes their updates.

The content below is taken directly from Crosspoint and aligns with CrossDink's vision as well.

## 1. Core Mission

To provide a lightweight, high-performance firmware that maximizes the potential of the X4 Pro, X4 Classic and Sticky, prioritizing legibility and usability over "swiss-army-knife" functionality.

## 2. Scope

### In-Scope

*These are features that directly improve the primary purpose of the device.*

* **User Experience:** E.g. User-friendly interfaces, and interactions, both inside the reader and navigating the
  firmware. This includes things like button mapping, book loading, and book navigation like bookmarks.
* **Document Rendering:** E.g. Support for rendering documents (primarily EPUB) and improvements to the rendering
  engine.
* **Format Optimization:** E.g. Efficiently parsing EPUB (CSS/Images) and other documents within the device's
  capabilities.
* **Typography & Legibility:** E.g. Custom font support, hyphenation engines, and adjustable line spacing.
* **E-Ink Driver Refinement:** E.g. Reducing full-screen flashes (ghosting management) and improving general rendering.
* **Library Management:** E.g. Simple, intuitive ways to organize and navigate a collection of books.
* **Local Transfer:** E.g. Simple, "pull" based book loading via a basic web-server or public and widely-used standards.
* **Language Support:** E.g. Support for multiple languages both in the reader and in the interfaces.
* **Reference Tools:** E.g. Local dictionary lookup. Providing quick, offline definitions to enhance comprehension
  without breaking focus.
* **Clock Display (device dependent):**

| Device | Scope |
| -- | -- |
| X4 Pro, X4 Classic, Sticky | Each uses a dedicated RTC chip (a BM8563 on the X4 Pro and X4 Classic, a PCF8563 on the Sticky) rather than the ESP32's internal RTC, so it keeps time across sleep cycles and can be treated as a wall clock. The Wi-Fi screen sets it over NTP when it was never set or has lost its time (e.g. a fully drained battery), and Settings can re-sync it on demand. |

### Out-of-Scope

*These items are rejected because they compromise the device's stability or mission.*

* **Interactive Apps:** No Notepads, Calculators, or Games. This is a reader, not a PDA.
* **Active Connectivity:** No RSS readers, News aggregators, or Web browsers. Background Wi-Fi tasks drain the battery and compete for CPU time with the main loop, which runs on the same core.
* **Media Playback:** No Audio players or Audiobooks.
* **Complex Annotation:** No typed out notes. These features are better suited for devices with better input capabilities and more powerful chips.

### In-scope — Technically Unsupported

*These features align with Crosspoint's goals but are impractical on the current hardware or produce poor UX.*

* **PDF Rendering:** PDFs are fixed-layout documents, so rendering them requires displaying pages as images rather than reflowable text — resulting in constant panning and zooming that makes for a poor reading experience on e-ink.

## 3. Idea Evaluation

While I appreciate the desire to add new and exciting features to Crosspoint Reader, Crosspoint Reader is designed to be a lightweight, reliable, and performant e-reader. Things which distract or compromise the device's core mission will not be accepted. As a guiding question, consider if your idea improve the "core reading experience" for the average user,
and, critically, not distract from that reading experience.
