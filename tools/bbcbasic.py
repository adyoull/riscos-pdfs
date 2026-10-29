#!/usr/bin/env python3
"""BBC BASIC V (RISC OS) detokeniser / tokeniser.

  bbcbasic.py detok IN OUT     tokenised -> text ("  10 PRINT ..." per line)
  bbcbasic.py tok   IN OUT     text -> tokenised

Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0.
Text format: each line "<number> <content>" (number optional -> auto +10).
"""
import sys, re

MAIN = {
0x7F:'OTHERWISE',
0x80:'AND',0x81:'DIV',0x82:'EOR',0x83:'MOD',0x84:'OR',0x85:'ERROR',0x86:'LINE',0x87:'OFF',
0x88:'STEP',0x89:'SPC',0x8A:'TAB(',0x8B:'ELSE',0x8C:'THEN',0x8E:'OPENIN',0x8F:'PTR',
0x90:'PAGE',0x91:'TIME',0x92:'LOMEM',0x93:'HIMEM',0x94:'ABS',0x95:'ACS',0x96:'ADVAL',0x97:'ASC',
0x98:'ASN',0x99:'ATN',0x9A:'BGET',0x9B:'COS',0x9C:'COUNT',0x9D:'DEG',0x9E:'ERL',0x9F:'ERR',
0xA0:'EVAL',0xA1:'EXP',0xA2:'EXT',0xA3:'FALSE',0xA4:'FN',0xA5:'GET',0xA6:'INKEY',0xA7:'INSTR(',
0xA8:'INT',0xA9:'LEN',0xAA:'LN',0xAB:'LOG',0xAC:'NOT',0xAD:'OPENUP',0xAE:'OPENOUT',0xAF:'PI',
0xB0:'POINT(',0xB1:'POS',0xB2:'RAD',0xB3:'RND',0xB4:'SGN',0xB5:'SIN',0xB6:'SQR',0xB7:'TAN',
0xB8:'TO',0xB9:'TRUE',0xBA:'USR',0xBB:'VAL',0xBC:'VPOS',0xBD:'CHR$',0xBE:'GET$',0xBF:'INKEY$',
0xC0:'LEFT$(',0xC1:'MID$(',0xC2:'RIGHT$(',0xC3:'STR$',0xC4:'STRING$(',0xC5:'EOF',
0xC9:'WHEN',0xCA:'OF',0xCB:'ENDCASE',0xCC:'ELSE',0xCD:'ENDIF',0xCE:'ENDWHILE',0xCF:'PTR',
0xD0:'PAGE',0xD1:'TIME',0xD2:'LOMEM',0xD3:'HIMEM',0xD4:'SOUND',0xD5:'BPUT',0xD6:'CALL',0xD7:'CHAIN',
0xD8:'CLEAR',0xD9:'CLOSE',0xDA:'CLG',0xDB:'CLS',0xDC:'DATA',0xDD:'DEF',0xDE:'DIM',0xDF:'DRAW',
0xE0:'END',0xE1:'ENDPROC',0xE2:'ENVELOPE',0xE3:'FOR',0xE4:'GOSUB',0xE5:'GOTO',0xE6:'GCOL',0xE7:'IF',
0xE8:'INPUT',0xE9:'LET',0xEA:'LOCAL',0xEB:'MODE',0xEC:'MOVE',0xED:'NEXT',0xEE:'ON',0xEF:'VDU',
0xF0:'PLOT',0xF1:'PRINT',0xF2:'PROC',0xF3:'READ',0xF4:'REM',0xF5:'REPEAT',0xF6:'REPORT',0xF7:'RESTORE',
0xF8:'RETURN',0xF9:'RUN',0xFA:'STOP',0xFB:'COLOUR',0xFC:'TRACE',0xFD:'UNTIL',0xFE:'WIDTH',0xFF:'OSCLI'}
EXT = {
0xC6:{0x8E:'SUM',0x8F:'BEAT'},
0xC7:{0x8E:'APPEND',0x8F:'AUTO',0x90:'CRUNCH',0x91:'DELETE',0x92:'EDIT',0x93:'HELP',0x94:'LIST',
      0x95:'LOAD',0x96:'LVAR',0x97:'NEW',0x98:'OLD',0x99:'RENUMBER',0x9A:'SAVE',0x9B:'TEXTLOAD',
      0x9C:'TEXTSAVE',0x9D:'TWIN',0x9E:'TWINO',0x9F:'INSTALL'},
0xC8:{0x8E:'CASE',0x8F:'CIRCLE',0x90:'FILL',0x91:'ORIGIN',0x92:'POINT',0x93:'RECTANGLE',0x94:'SWAP',
      0x95:'WHILE',0x96:'WAIT',0x97:'MOUSE',0x98:'QUIT',0x99:'SYS',0x9A:'INSTALL',0x9B:'LIBRARY',
      0x9C:'TINT',0x9D:'ELLIPSE',0x9E:'BEATS',0x9F:'TEMPO',0xA0:'VOICES',0xA1:'VOICE',0xA2:'STEREO',
      0xA3:'OVERLAY',0xA4:'MANDEL',0xA5:'PRIVATE',0xA6:'EXIT'}}

def enc_lineno(n):
    lo, hi = n & 0xFF, (n >> 8) & 0xFF
    b1 = (((lo & 0xC0) >> 2) | ((hi & 0xC0) >> 4)) ^ 0x54
    return bytes([0x8D, b1, (lo & 0x3F) | 0x40, (hi & 0x3F) | 0x40])

def detok(data):
    out = []; p = 0
    while True:
        assert data[p] == 0x0D, 'bad line start at %d' % p
        if data[p+1] == 0xFF: break
        num = data[p+1]*256 + data[p+2]; ln = data[p+3]
        body = data[p+4:p+ln]; p += ln
        s = []; i = 0; q = False; lit = False
        while i < len(body):
            c = body[i]
            if lit or q:
                if c == 0x22: q = not q
                s.append(chr(c) if c < 0x80 else chr(c)); i += 1; continue
            if c == 0x22: q = True; s.append('"'); i += 1; continue
            if c == 0x8D:
                b1, b2, b3 = body[i+1:i+4]; b1 ^= 0x54
                lo = (b2 & 0x3F) | ((b1 << 2) & 0xC0); hi = (b3 & 0x3F) | ((b1 << 4) & 0xC0)
                s.append(str(lo + hi*256)); i += 4; continue
            if c in EXT:
                s.append(EXT[c][body[i+1]]); i += 2; continue
            if c >= 0x7F:
                t = MAIN[c]; s.append(t); i += 1
                if t in ('REM', 'DATA'): lit = True
                continue
            s.append(chr(c)); i += 1
        out.append('%d %s' % (num, ''.join(s)))
    return out

# ---- tokeniser ----
# token lists: (word, byte(s), kind). Statement-position variants for PTR/PAGE/TIME/LOMEM/HIMEM.
FUNC_FORM = {'PTR':0x8F,'PAGE':0x90,'TIME':0x91,'LOMEM':0x92,'HIMEM':0x93}
STMT_FORM = {'PTR':0xCF,'PAGE':0xD0,'TIME':0xD1,'LOMEM':0xD2,'HIMEM':0xD3}
WORDS = {}
for b, w in MAIN.items():
    if w in FUNC_FORM or b in (0xCC,): continue
    WORDS.setdefault(w, bytes([b]))
for w in FUNC_FORM: WORDS[w] = None
for pre, tbl in EXT.items():
    for b, w in tbl.items():
        if pre == 0xC7 and w == 'INSTALL': continue  # statement INSTALL is C8 9A
        WORDS.setdefault(w, bytes([pre, b]))
# words that are tokenised only if not followed by an identifier char ("conditional")
COND = {'END','PI','TRUE','FALSE','ERL','ERR','TIME','PAGE','PTR','LOMEM','HIMEM','COUNT','POS','VPOS',
        'GET','GET$','INKEY','INKEY$','RND','CLS','CLG','CLEAR','REPORT','RETURN','STOP','TRACE','ENDPROC',
        'ENDIF','ENDWHILE','ENDCASE','OTHERWISE','NEW','OLD','QUIT','WAIT','EXIT','SUM','BEAT','BEATS','TEMPO','VOICES','WIDTH','LINE'}
# words after which a line number may follow
LINENO_AFTER = {'GOTO','GOSUB','RESTORE','THEN','ELSE','TRACE','RUN','DELETE','LIST','RENUMBER'}
SORTED = sorted(WORDS, key=len, reverse=True)
IDC = re.compile(r'[A-Za-z0-9_`]')

in_asm = [False]
def tok_line(text):
    out = bytearray(); i = 0; n = len(text)
    stmt_start = True; lineno_ok = False; prev_fn_proc = False
    while i < n:
        c = text[i]
        if c == '"':
            j = text.index('"', i+1) if '"' in text[i+1:] else n-1
            out += text[i:j+1].encode('latin-1'); i = j+1
            stmt_start = False; lineno_ok = False; continue
        if lineno_ok and c.isdigit():
            j = i
            while j < n and text[j].isdigit(): j += 1
            out += enc_lineno(int(text[i:j])); i = j; continue
        if c == ',' and lineno_ok:
            out += b','; i += 1; continue
        if c == ' ':
            out += b' '; i += 1; continue
        lineno_ok = False
        if c.isalpha() and c.isupper():
            for w in SORTED:
                if text.startswith(w, i):
                    end = i + len(w)
                    nxt = text[end] if end < n else ''
                    if w in COND and nxt and IDC.match(nxt): continue
                    if w == 'ELSE':
                        out.append(0xCC if out.strip(b' ') == b'' else 0x8B)
                    if w in FUNC_FORM:
                        out.append(STMT_FORM[w] if stmt_start else FUNC_FORM[w])
                    elif w != 'ELSE':
                        out += WORDS[w]
                    i = end
                    if w in ('REM', 'DATA'):
                        out += text[i:].encode('latin-1'); return bytes(out)
                    if w in ('FN', 'PROC'):
                        j = i
                        while j < n and IDC.match(text[j]): j += 1
                        out += text[i:j].encode('latin-1'); i = j; stmt_start = False; break
                    stmt_start = w in ('THEN', 'ELSE', 'REPEAT', 'OTHERWISE')
                    lineno_ok = w in LINENO_AFTER
                    break
            else:
                j = i
                while j < n and IDC.match(text[j]): j += 1
                out += text[i:j].encode('latin-1'); i = j; stmt_start = False
            continue
        if c.isdigit() or c == '.':
            j = i
            while j < n and (text[j].isdigit() or text[j] == '.'): j += 1
            if j < n and text[j] == 'E' and j+1 < n and (text[j+1].isdigit() or text[j+1] in '+-'):
                j += 2
                while j < n and text[j].isdigit(): j += 1
            out += text[i:j].encode('latin-1'); i = j; stmt_start = False; continue
        if c == '[':
            in_asm[0] = True
        if c == ']':
            in_asm[0] = False
        if in_asm[0] and c in ';\\':
            j = text.find(':', i)
            if j < 0: j = n
            out += text[i:j].encode('latin-1'); i = j; continue
        if IDC.match(c):
            j = i
            while j < n and IDC.match(text[j]): j += 1
            out += text[i:j].encode('latin-1'); i = j; stmt_start = False; continue
        if c == '&':
            j = i + 1
            while j < n and text[j] in '0123456789ABCDEFabcdef': j += 1
            out += text[i:j].encode('latin-1'); i = j; stmt_start = False; continue
        if c == ':':
            out += b':'; i += 1; stmt_start = True; continue
        if c == '*' and stmt_start:
            out += text[i:].encode('latin-1'); return bytes(out)
        out += c.encode('latin-1'); i += 1
        stmt_start = False
    return bytes(out)

def tok(lines):
    out = bytearray(); last = 0; in_asm[0] = False
    for L in lines:
        m = re.match(r'\s*(\d+) ?(.*)$', L)
        if m: num, body = int(m.group(1)), m.group(2)
        else: num, body = last + 10, L
        last = num
        b = tok_line(body)
        if len(b) + 4 > 255: raise SystemExit('line %d too long' % num)
        out += bytes([0x0D, num >> 8, num & 0xFF, len(b) + 4]) + b
    out += b'\r\xff'
    return bytes(out)

if __name__ == '__main__':
    mode, a, b = sys.argv[1:4]
    if mode == 'detok':
        open(b, 'w', encoding='latin-1').write('\n'.join(detok(open(a, 'rb').read())) + '\n')
    else:
        open(b, 'wb').write(tok(open(a, encoding='latin-1').read().splitlines()))
