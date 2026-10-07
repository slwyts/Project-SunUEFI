"""Recognize nonfunctional source footers without removing executable text."""


def normalize_source(text):
    text = text.replace('\r\n', '\n')
    if '\r' in text:
        raise ValueError('Source contains a lone CR')
    return text


def split_source_tail(text, *, inf=False):
    """Split only complete standalone footer comments and ASCII whitespace.

    The returned body is still checked in full. Backslashes and trigraph
    continuations are excluded because preprocessing runs before comments.
    INF supports its own # comment syntax; C/ASM never treats # as a comment.
    """
    text = normalize_source(text)
    end = len(text)
    while True:
        end = len(text[:end].rstrip(' \t\n'))
        line = text.rfind('\n', 0, end) + 1
        last = text[line:end].lstrip(' \t')
        start = None
        if last.startswith('#' if inf else '//'):
            start = line
        elif not inf and text[:end].endswith('*/'):
            opening = text.rfind('/*', 0, end - 2)
            beginning = text.rfind('\n', 0, opening) + 1
            if opening >= 0 and not text[beginning:opening].strip(' \t') and \
                    text.find('*/', opening + 2) == end - 2:
                start = beginning
        if start is None:
            return text[:end], text[end:]
        comment = text[start:end]
        if '\\' in comment or '??/' in comment:
            raise ValueError('Source footer may not contain line continuations')
        end = start
