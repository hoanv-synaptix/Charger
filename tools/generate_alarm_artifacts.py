"""Generate the review artifacts from the canonical alarm Markdown table."""

from html import escape
from pathlib import Path

import openpyxl
from openpyxl.styles import Alignment, Font, PatternFill


ROOT = Path(__file__).resolve().parents[1]
MARKDOWN = ROOT / "docs" / "BANG_MA_LOI_HE_THONG_.md"
HTML = ROOT / "docs" / "BANG_MA_LOI_HE_THONG.html"
XLSX = ROOT / "docs" / "BANG_MA_LOI_HE_THONG.xlsx"


def table_rows():
    rows = []
    in_table = False
    for line in MARKDOWN.read_text(encoding="utf-8").splitlines():
        if line.startswith("| Lỗi từ |"):
            in_table = True
            rows.append([part.strip() for part in line.strip("|").split("|")])
            continue
        if not in_table:
            continue
        if not line.startswith("|"):
            # Allow the conventional blank line between a Markdown header
            # and its separator row.
            if not line.strip():
                continue
            break
        if line.startswith("| :---"):
            continue
        parts = [part.strip() for part in line.strip("|").split("|")]
        if len(parts) == 7:
            rows.append(parts)
    return rows


def md_to_html(value):
    value = escape(value, quote=True)
    value = value.replace("&lt;br&gt;", "<br>")
    value = value.replace("`", "")
    value = value.replace("**", "")
    return value


def write_html(rows):
    headers = rows[0]
    body = []
    for row in rows[1:]:
        body.append("<tr>" + "".join(f"<td>{md_to_html(cell)}</td>" for cell in row) + "</tr>")
    header = "".join(f"<th>{md_to_html(cell)}</th>" for cell in headers)
    HTML.write_text(
        "<!doctype html>\n<html lang=\"vi\"><head><meta charset=\"utf-8\">"
        "<title>Bảng mã lỗi hệ thống</title><style>"
        "body{font-family:Arial,sans-serif;margin:24px}table{border-collapse:collapse;width:100%;font-size:13px}"
        "th,td{border:1px solid #bbb;padding:7px;vertical-align:top}th{background:#e8eef7}"
        "td:nth-child(3){white-space:nowrap;font-weight:700}"
        "</style></head><body><h1>Bảng mã lỗi hệ thống</h1>"
        "<p>Generated artifact. Canonical source: <code>BANG_MA_LOI_HE_THONG_.md</code>.</p>"
        f"<table><thead><tr>{header}</tr></thead><tbody>{''.join(body)}</tbody></table>"
        "</body></html>\n",
        encoding="utf-8",
    )


def write_xlsx(rows):
    workbook = openpyxl.load_workbook(XLSX) if XLSX.exists() else openpyxl.Workbook()
    old = workbook.worksheets[0]
    title = old.title
    workbook.remove(old)
    sheet = workbook.create_sheet(title, 0)
    for row in rows:
        sheet.append([cell.replace("<br>", "\n").replace("**", "").replace("`", "") for cell in row])
    for cell in sheet[1]:
        cell.font = Font(bold=True, color="FFFFFF")
        cell.fill = PatternFill("solid", fgColor="1F4E78")
        cell.alignment = Alignment(horizontal="center", vertical="center", wrap_text=True)
    for row in sheet.iter_rows(min_row=2):
        for cell in row:
            cell.alignment = Alignment(vertical="top", wrap_text=True)
    widths = [18, 30, 12, 30, 28, 42, 86]
    for index, width in enumerate(widths, 1):
        sheet.column_dimensions[chr(64 + index)].width = width
    sheet.freeze_panes = "A2"
    sheet.auto_filter.ref = sheet.dimensions
    workbook.save(XLSX)


def main():
    rows = table_rows()
    if not rows or len(rows[0]) != 7:
        raise SystemExit("canonical alarm table not found")
    write_xlsx(rows)


if __name__ == "__main__":
    main()
