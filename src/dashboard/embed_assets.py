#!/usr/bin/env python3
import sys
from pathlib import Path


def raw_literal(name: str, text: str, index: int) -> str:
    delimiter = f"BRD{index}"
    while f"){delimiter}\"" in text:
        delimiter += "X"
    return f'const char {name}[] = R"{delimiter}({text}){delimiter}";\n'


def main() -> int:
    if len(sys.argv) != 5:
        print("usage: embed_assets.py <index.html> <styles.css> <app.js> <output.cpp>", file=sys.stderr)
        return 2

    index_html = Path(sys.argv[1]).read_text()
    styles_css = Path(sys.argv[2]).read_text()
    app_js = Path(sys.argv[3]).read_text()
    output = Path(sys.argv[4])
    output.parent.mkdir(parents=True, exist_ok=True)

    body = [
        "#include \"assets.h\"\n\n",
        "namespace {\n",
        raw_literal("kIndexHtml", index_html, 0),
        raw_literal("kStylesCss", styles_css, 1),
        raw_literal("kAppJs", app_js, 2),
        "} // namespace\n\n",
        "namespace dashboard_assets {\n",
        "std::string_view index_html() { return kIndexHtml; }\n",
        "std::string_view styles_css() { return kStylesCss; }\n",
        "std::string_view app_js() { return kAppJs; }\n",
        "} // namespace dashboard_assets\n",
    ]
    output.write_text("".join(body))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
