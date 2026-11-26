# -*- coding: utf-8 -*-
"""把本项目用的简化 Markdown 转成中文公文风格的 docx（A4、3.2cm 页边距、五号宋体）。"""

import re
import sys

from docx import Document
from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_LINE_SPACING
from docx.oxml.ns import qn
from docx.shared import Cm, Pt, RGBColor

CN_BODY = '宋体'
CN_HEAD = '微软雅黑'
EN_FONT = 'Times New Roman'


def set_run_font(run, cn=CN_BODY, size=10.5, bold=False, color=None):
    run.font.size = Pt(size)
    run.font.bold = bold
    run.font.name = EN_FONT
    rpr = run._element.get_or_add_rPr()
    rfonts = rpr.find(qn('w:rFonts'))
    if rfonts is None:
        rfonts = rpr.makeelement(qn('w:rFonts'), {})
        rpr.append(rfonts)
    rfonts.set(qn('w:eastAsia'), cn)
    rfonts.set(qn('w:ascii'), EN_FONT)
    rfonts.set(qn('w:hAnsi'), EN_FONT)
    if color is not None:
        run.font.color.rgb = color


def add_rich_text(paragraph, text, size=10.5, bold=False, cn=CN_BODY):
    """支持 **粗体** 与 `等宽` 的行内解析。"""
    tokens = re.split(r'(\*\*.+?\*\*|`.+?`)', text)
    for token in tokens:
        if not token:
            continue
        if token.startswith('**') and token.endswith('**'):
            run = paragraph.add_run(token[2:-2])
            set_run_font(run, cn=cn, size=size, bold=True)
        elif token.startswith('`') and token.endswith('`'):
            run = paragraph.add_run(token[1:-1])
            set_run_font(run, cn=cn, size=size - 0.5, bold=bold)
            run.font.name = 'Consolas'
            run._element.get_or_add_rPr().find(qn('w:rFonts')).set(qn('w:eastAsia'), 'Consolas')
        else:
            run = paragraph.add_run(token)
            set_run_font(run, cn=cn, size=size, bold=bold)


def style_body(paragraph, space_before=0, space_after=6, line=1.42):
    fmt = paragraph.paragraph_format
    fmt.space_before = Pt(space_before)
    fmt.space_after = Pt(space_after)
    fmt.line_spacing = line
    fmt.line_spacing_rule = WD_LINE_SPACING.MULTIPLE


def visible_len(text):
    return sum(2 if ord(ch) > 127 else 1 for ch in re.sub(r'\*\*|`', '', text))


def add_table(doc, rows):
    cols = len(rows[0])
    table = doc.add_table(rows=len(rows), cols=cols)
    table.style = 'Table Grid'
    table.alignment = WD_TABLE_ALIGNMENT.CENTER
    table.autofit = False

    # 行内不跨页拆分；首行作为标题行在后续页重复
    for r, row in enumerate(table.rows):
        trpr = row._tr.get_or_add_trPr()
        trpr.append(trpr.makeelement(qn('w:cantSplit'), {qn('w:val'): 'true'}))
        if r == 0:
            trpr.append(trpr.makeelement(qn('w:tblHeader'), {qn('w:val'): 'true'}))
    for r, row in enumerate(rows):
        for c in range(cols):
            text = row[c] if c < len(row) else ''
            cell = table.cell(r, c)
            cell.text = ''
            para = cell.paragraphs[0]
            para.alignment = WD_ALIGN_PARAGRAPH.LEFT
            style_body(para, space_before=1, space_after=1, line=1.15)
            add_rich_text(para, text, size=9.5, bold=(r == 0), cn=CN_BODY)
            if r == 0:
                for run in para.runs:
                    run.font.bold = True
    # 表头底纹
    for c in range(cols):
        cell = table.cell(0, c)
        tcpr = cell._tc.get_or_add_tcPr()
        shd = tcpr.makeelement(qn('w:shd'), {qn('w:val'): 'clear', qn('w:fill'): 'E8EEF7'})
        tcpr.append(shd)

    # 按列内容宽度分配总宽 14.6cm
    total = 14.6
    weights = []
    for c in range(cols):
        w = max(visible_len(r[c]) if c < len(r) else 0 for r in rows)
        weights.append(max(3.0, min(w, 34.0)))
    base = sum(weights)
    widths = [max(1.2, total * w / base) for w in weights]
    scale = total / sum(widths)
    widths = [w * scale for w in widths]
    for c, width in enumerate(widths):
        for r in range(len(rows)):
            table.cell(r, c).width = Cm(width)
    return table


def build(md_path, out_path, title=None):
    with open(md_path, encoding='utf-8') as fh:
        lines = fh.read().split('\n')

    doc = Document()
    sec = doc.sections[0]
    sec.page_width = Cm(21.0)
    sec.page_height = Cm(29.7)
    sec.left_margin = Cm(3.2)
    sec.right_margin = Cm(3.2)
    sec.top_margin = Cm(2.6)
    sec.bottom_margin = Cm(2.6)

    normal = doc.styles['Normal']
    normal.font.name = EN_FONT
    normal.font.size = Pt(10.5)
    normal.element.get_or_add_rPr().get_or_add_rFonts().set(qn('w:eastAsia'), CN_BODY)

    # 页脚页码
    footer_para = sec.footer.paragraphs[0]
    footer_para.alignment = WD_ALIGN_PARAGRAPH.CENTER
    run = footer_para.add_run()
    set_run_font(run, size=9)
    fld = run._element.makeelement(qn('w:fldSimple'), {qn('w:instr'): 'PAGE'})
    run._element.addnext(fld)

    i = 0
    first_h1_done = False
    n_table = n_para = 0
    while i < len(lines):
        raw = lines[i]
        line = raw.rstrip()
        stripped = line.strip()

        if not stripped:
            i += 1
            continue

        if stripped in ('<!-- pb -->', '\\pagebreak'):
            doc.add_page_break()
            i += 1
            continue

        if stripped.startswith('```'):
            block = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith('```'):
                block.append(lines[i])
                i += 1
            i += 1
            for bl in block:
                para = doc.add_paragraph()
                style_body(para, space_before=0, space_after=0, line=1.15)
                para.paragraph_format.left_indent = Cm(0.5)
                para.paragraph_format.right_indent = Cm(0.5)
                ppr = para._p.get_or_add_pPr()
                ppr.append(ppr.makeelement(qn('w:shd'), {qn('w:val'): 'clear', qn('w:fill'): 'F4F6F8'}))
                run = para.add_run(bl)
                run.font.name = 'Consolas'
                run.font.size = Pt(9)
                run._element.get_or_add_rPr().get_or_add_rFonts().set(qn('w:eastAsia'), 'Consolas')
            doc.add_paragraph()
            continue

        if stripped.startswith('# '):
            text = stripped[2:].strip()
            para = doc.add_paragraph()
            para.alignment = WD_ALIGN_PARAGRAPH.CENTER
            style_body(para, space_before=0, space_after=18, line=1.2)
            run = para.add_run(text)
            set_run_font(run, cn=CN_HEAD, size=18, bold=True)
            first_h1_done = True
            i += 1
            continue

        if stripped.startswith('## '):
            text = stripped[3:].strip()
            para = doc.add_paragraph(style='Heading 1')
            style_body(para, space_before=14, space_after=8, line=1.3)
            para.paragraph_format.keep_with_next = True
            run = para.add_run(text)
            set_run_font(run, cn=CN_HEAD, size=14, bold=True, color=RGBColor(0x1F, 0x3A, 0x5F))
            i += 1
            continue

        if stripped.startswith('### '):
            text = stripped[4:].strip()
            para = doc.add_paragraph(style='Heading 2')
            style_body(para, space_before=10, space_after=6, line=1.3)
            para.paragraph_format.keep_with_next = True
            run = para.add_run(text)
            set_run_font(run, cn=CN_HEAD, size=12, bold=True, color=RGBColor(0x22, 0x22, 0x22))
            i += 1
            continue

        if stripped in ('---', '***', '___'):
            para = doc.add_paragraph()
            style_body(para, space_before=2, space_after=8, line=1.0)
            ppr = para._p.get_or_add_pPr()
            pbdr = ppr.makeelement(qn('w:pBdr'), {})
            bottom = ppr.makeelement(qn('w:bottom'), {
                qn('w:val'): 'single', qn('w:sz'): '4',
                qn('w:space'): '1', qn('w:color'): 'C8D2E0'})
            pbdr.append(bottom)
            ppr.append(pbdr)
            i += 1
            continue

        if stripped.startswith('>'):
            text = stripped.lstrip('>').strip()
            para = doc.add_paragraph()
            style_body(para, space_before=3, space_after=5, line=1.35)
            para.paragraph_format.left_indent = Cm(0.55)
            para.paragraph_format.right_indent = Cm(0.3)
            ppr = para._p.get_or_add_pPr()
            pbdr = ppr.makeelement(qn('w:pBdr'), {})
            left = ppr.makeelement(qn('w:left'), {
                qn('w:val'): 'single', qn('w:sz'): '12',
                qn('w:space'): '6', qn('w:color'): '8FA9C7'})
            pbdr.append(left)
            ppr.append(pbdr)
            ppr.append(ppr.makeelement(qn('w:shd'), {qn('w:val'): 'clear', qn('w:fill'): 'F5F7FA'}))
            add_rich_text(para, text, size=10)
            n_para += 1
            i += 1
            continue

        if stripped.startswith('|'):
            rows = []
            while i < len(lines) and lines[i].strip().startswith('|'):
                cells = [c.strip() for c in lines[i].strip().strip('|').split('|')]
                if not all(re.fullmatch(r':?-{2,}:?', c) for c in cells if c):
                    rows.append(cells)
                i += 1
            if rows:
                add_table(doc, rows)
                n_table += 1
                tail = doc.add_paragraph()
                style_body(tail, space_before=0, space_after=4, line=1.0)
            continue

        if stripped.startswith('- '):
            para = doc.add_paragraph()
            style_body(para, space_before=0, space_after=3, line=1.42)
            para.paragraph_format.left_indent = Cm(1.1)
            para.paragraph_format.first_line_indent = Cm(-0.36)
            add_rich_text(para, '· ' + stripped[2:].strip(), size=10.5)
            n_para += 1
            i += 1
            continue

        para = doc.add_paragraph()
        style_body(para, space_before=0, space_after=6, line=1.42)
        para.paragraph_format.first_line_indent = Cm(0.74)
        add_rich_text(para, stripped, size=10.5)
        n_para += 1
        i += 1

    doc.core_properties.title = title or (lines[0].lstrip('# ').strip() if lines else '')
    doc.core_properties.author = ''
    doc.save(out_path)
    print('saved %s paragraphs=%d tables=%d' % (out_path, n_para, n_table))


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
