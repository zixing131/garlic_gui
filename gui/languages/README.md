# Garlic GUI language files

Select a language in Preferences → Appearance, then restart the GUI. The loader scans
`languages/` beside the executable; on macOS it also checks `languages/` beside
the `.app` before falling back to `Contents/Resources/languages/`.
The source language is Simplified Chinese (`zh_CN`).

To add a language, copy a JSON catalog, name it `<locale>.json`, and change:

```json
{
  "locale": "de",
  "name": "Deutsch",
  "messages": {"文件": "Datei", "显示 %1 / %2 处引用%3": "%1 / %2 Verwendungen%3"}
}
```

Use UTF-8 and preserve `%1`, `%2`, `%n`, newline characters, and other placeholders.
Keys are original Chinese strings used by Qt `tr()`. Missing or empty entries fall
back to the original text. Locale identifiers accept two or three lowercase letters
and optional underscore-separated region/script components. No compilation or Qt
Linguist installation is needed. Invalid JSON and catalogs larger than 4 MiB are ignored.

The initial catalogs translate the main menus, preferences controls, navigation,
search, references, and scripting controls. Detailed engine diagnostics and some
specialist explanations currently retain their original language. Add matching
source keys to extend coverage. Engine/CLI output and source code are not translated.

On macOS, put custom catalogs in `languages/` beside `Garlic.app` to override
bundled catalogs without modifying the app bundle.
