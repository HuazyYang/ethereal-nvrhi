#!/usr/bin/env python3
"""ABI lint for the public NVRHI headers.

The public interfaces must look the same to MSVC (and clang-cl) and to MinGW GCC/Clang. This script checks the
rules that keep them so, on the headers under include/nvrhi:

  R1  No class, struct or union by value in a virtual function of an interface: neither as the return type nor
      as a parameter. References, pointers, enums and scalar typedefs are fine (Vulkan handles are pointers).
  R2  No virtual destructor in an interface.
  R3  No two virtual functions with the same name in an interface (no overloads).
  R5  Every virtual function of an interface is noexcept.
  R4  No base class on a data struct (anything but an interface) in a public API header.
  T1  No SpinLock / Signal / LFStack / SharedSpinLock / MonoPtr / STDAllocator in a public API header.
  E   Export rule: no NVRHI_API / NVRHI_C_API in a class scope, and no non-virtual member function or
      constructor declared without a body (its definition would be an exported or missing C++ symbol), unless
      the same header defines it out of class.

An interface is a class or struct whose name is I followed by an upper-case letter. The public API headers are
nvrhi.h, d3d11.h, d3d12.h, vulkan.h, validation.h, utils.h, common/*.h and core/containers.h (exempt from R4: the
containers are allowed to use helper bases). core/foundation.h and core/threading.h are implementation-side
helpers and are not scanned.

usage: abi_lint.py <include/nvrhi directory>
       abi_lint.py --self-test <include/nvrhi directory>
"""
import os
import re
import shutil
import sys
import tempfile

EXCLUDED = {'core/foundation.h', 'core/threading.h'}
API_HEADERS = {'nvrhi.h', 'd3d11.h', 'd3d12.h', 'vulkan.h', 'validation.h', 'utils.h'}
R4_EXEMPT = {'core/containers.h'}
FORBIDDEN_TYPES = ['SpinLock', 'Signal', 'LFStack', 'SharedSpinLock', 'MonoPtr', 'STDAllocator']
EXPORT_MACROS = ['NVRHI_API', 'NVRHI_C_API', 'NVRHI_CORE_API', 'NVRHI_CORE_C_API']
# (header, class, member) declared without a body on purpose: none at the moment. Declaration-only helpers that
# are used in decltype only would go here.
BODYLESS_EXEMPT = set()

BUILTIN = {
    'void', 'bool', 'char', 'short', 'int', 'long', 'float', 'double', 'unsigned', 'signed', 'size_t', 'ptrdiff_t',
    'int8_t', 'int16_t', 'int32_t', 'int64_t', 'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t', 'intptr_t',
    'uintptr_t', 'nullptr_t', 'wchar_t', 'char16_t', 'char32_t',
}
# Scalars and pointers from the platform headers that the public interfaces use.
EXTERNAL_SCALARS = {
    'HANDLE', 'HRESULT', 'UINT', 'UINT64', 'SIZE_T', 'BOOL', 'DXGI_FORMAT', 'D3D12_GPU_VIRTUAL_ADDRESS',
    'VkFormat', 'VkResult',
    # Vulkan handles: dispatchable handles are pointers, non-dispatchable ones are pointers on 64-bit targets.
    'VkInstance', 'VkPhysicalDevice', 'VkDevice', 'VkQueue', 'VkCommandBuffer', 'VkSemaphore', 'VkFence',
    'VkDeviceMemory', 'VkBuffer', 'VkImage', 'VkEvent', 'VkQueryPool', 'VkBufferView', 'VkImageView',
    'VkShaderModule', 'VkPipelineCache', 'VkPipelineLayout', 'VkRenderPass', 'VkPipeline', 'VkDescriptorSetLayout',
    'VkSampler', 'VkDescriptorPool', 'VkDescriptorSet', 'VkFramebuffer', 'VkCommandPool',
    'VkAccelerationStructureKHR', 'VkMicromapEXT',
}
QUALIFIERS = {'const', 'volatile', 'struct', 'class', 'union', 'enum', 'typename', 'register'}


class Lint:
    def __init__(self):
        self.errors = []
        self.enums = set()
        self.interfaces = 0
        self.virtuals = 0
        self.classes = 0
        self.aliases = {}       # name -> target text

    def error(self, header, what):
        self.errors.append(f'{header}: {what}')


# ---------------------------------------------------------------- source preparation

def strip_source(text):
    """Removes comments, preprocessor directives (with continuations) and the contents of literals."""
    out = []
    i, n = 0, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if line_start and c in ' \t':
            out.append(c)
            i += 1
            continue
        if line_start and c == '#':
            # preprocessor directive, with backslash continuations
            while i < n:
                j = text.find('\n', i)
                j = n if j < 0 else j
                line = text[i:j]
                cont = line.rstrip().endswith('\\')
                out.append('\n' if j < n else '')
                i = j + 1
                if not cont:
                    break
            line_start = True
            continue
        line_start = False
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
            continue
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + c)
            i = j + 1
            continue
        if c == '\n':
            line_start = True
        out.append(c)
        i += 1
    text = ''.join(out)
    # Upper-case macro invocations (NVRHI_IID(...), NVRHI_DECLARE_UUID_TRAITS(...), ...): their expansion is not
    # visible here, and they have no semicolon of their own.
    macro = re.compile(r'\b[A-Z][A-Z0-9]*_[A-Z0-9_]*\s*\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)')
    return macro.sub(' ', text)


def match_close(text, i, open_ch, close_ch):
    depth = 0
    for j in range(i, len(text)):
        if text[j] == open_ch:
            depth += 1
        elif text[j] == close_ch:
            depth -= 1
            if depth == 0:
                return j
    raise ValueError(f'unbalanced {open_ch}{close_ch}')


FUNCTION_HEAD = re.compile(r'\)\s*(const\s*)?(volatile\s*)?(&{1,2}\s*)?(noexcept\s*(\([^()]*\))?\s*)?'
                           r'(override\s*|final\s*)*(->[^{]*)?$|\)\s*:.*$', re.S)


def parse_class_head(head):
    """(kind, name or None, base clause) of 'struct Name<args> final : bases'."""
    m = re.match(r'^(?:typedef\s+)?(struct|class|union)\s*(\w+)?', head)
    kind, name = m.group(1), m.group(2)
    rest = head[m.end():].strip()
    if rest.startswith('<'):
        rest = rest[match_close(rest, 0, '<', '>') + 1:].strip()
    rest = re.sub(r'^final\b', '', rest).strip()
    bases = rest[1:].strip() if rest.startswith(':') and not rest.startswith('::') else ''
    return kind, name, bases


def declaration_head(head):
    """The head of a declaration without access specifiers, attributes and a template<...> prefix."""
    head = re.sub(r'^\s*(?:(?:public|protected|private)\s*:\s*)+', '', head)
    head = re.sub(r'\[\[[^\]]*\]\]\s*', '', head).strip()
    if head.startswith('template'):
        lt = head.find('<')
        if lt >= 0:
            head = head[match_close(head, lt, '<', '>') + 1:].strip()
    return head


class Statement:
    def __init__(self, text, has_body):
        self.text = ' '.join(text.split())
        self.has_body = has_body


def parse_scope(lint, header, text, start, end, scope, class_name, classes, definitions):
    """Splits text[start:end] into statements. scope: 'namespace' or 'class'."""
    members = []
    i = start
    stmt_start = start
    paren = 0
    while i < end:
        c = text[i]
        if c == '(':
            paren += 1
        elif c == ')':
            paren -= 1
        elif c == ';' and paren == 0:
            members.append(Statement(text[stmt_start:i], False))
            stmt_start = i + 1
        elif c == '{' and paren == 0:
            head = text[stmt_start:i]
            close = match_close(text, i, '{', '}')
            head_s = head.strip()
            if re.search(r'\bnamespace\b', head_s) or re.match(r'^extern\s*""$', head_s):
                parse_scope(lint, header, text, i + 1, close, 'namespace', None, classes, definitions)
                stmt_start = close + 1
            elif re.search(r'\benum\b', head_s):
                # enum body: skip to the end of the declaration
                semi = text.index(';', close)
                stmt_start = semi + 1
                close = semi
            elif re.match(r'^(typedef\s+)?(struct|class|union)\b', declaration_head(head_s)):
                kind, name, bases = parse_class_head(declaration_head(head_s))
                full = f'{class_name}::{name}' if class_name and name else (name or f'{class_name}::<anon>')
                cls = {'name': name, 'full': full, 'kind': kind, 'bases': bases, 'members': [],
                       'header': header, 'template': head_s.startswith('template')}
                classes.append(cls)
                cls['members'] = parse_scope(lint, header, text, i + 1, close, 'class', full, classes, definitions)
                # a declarator may follow (anonymous union / struct members)
                rest = re.match(r'\s*[^;{}]*;', text[close + 1:end])
                if rest:
                    close = close + rest.end()
                stmt_start = close + 1
            elif FUNCTION_HEAD.search(head_s):
                members.append(Statement(head, True))
                if scope == 'namespace':
                    m = re.search(r'\b(\w+)\s*(<[^<>]*(?:<[^<>]*>[^<>]*)*>)?\s*::\s*(~?\w+)\s*\(', head)
                    if m:
                        definitions.add((m.group(1), m.group(3)))
                stmt_start = close + 1
            else:
                # brace initializer: part of the current statement
                pass
            i = close + 1
            continue
        i += 1
    return members


# ---------------------------------------------------------------- type analysis

def strip_templates(t):
    out, depth = [], 0
    for ch in t:
        if ch == '<':
            depth += 1
            if depth == 1:
                out.append('<>')
            continue
        if ch == '>':
            depth -= 1
            continue
        if depth == 0:
            out.append(ch)
    return ''.join(out)


def split_top(text, sep=','):
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch in '(<[{':
            depth += 1
        elif ch in ')>]}':
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def type_of_param(p):
    p = re.sub(r'\b_[A-Z][A-Za-z_]*_\s*(\([^()]*\))?', ' ', p)          # SAL annotations
    p = split_top(p, '=')[0] if '=' in p else p                          # default argument
    p = p.strip()
    if not p or p == 'void':
        return None
    t = strip_templates(p)
    words = re.findall(r'[A-Za-z_][\w:]*|[*&]+|<>', t)
    # drop the parameter name: the last identifier, if it follows a type
    idents = [w for w in words if re.match(r'[A-Za-z_]', w) and w not in QUALIFIERS]
    if len(idents) >= 2 and words[-1] == idents[-1] and not (len(idents) == 2 and idents[0] in ('unsigned', 'signed', 'long', 'short')):
        words = words[:-1]
    return ' '.join(words)


def resolve_by_value_class(lint, t, depth=0):
    """None if t is a pointer, reference, enum or scalar; else the name of the class type passed by value."""
    if '*' in t or '&' in t:
        return None
    words = [w for w in t.split() if w not in QUALIFIERS]
    if not words:
        return None
    if '<>' in words:
        return t
    if all(w.split('::')[-1] in BUILTIN for w in words):
        return None
    if len(words) != 1:
        return t
    name = words[0].split('::')[-1]
    if name in BUILTIN or name in EXTERNAL_SCALARS or name in lint.enums:
        return None
    if re.match(r'^Vk\w*Flags\w*$', name):
        return None
    if name in lint.aliases and depth < 8:
        target = lint.aliases[name]
        if target is None:      # function pointer
            return None
        return resolve_by_value_class(lint, strip_templates(target), depth + 1) and name
    return name


# ---------------------------------------------------------------- checks

def collect_types(lint, sources):
    for header, text in sources.items():
        for m in re.finditer(r'\benum\s+(?:class\s+|struct\s+)?(\w+)\s*(?::[^{;]*)?[{;]', text):
            lint.enums.add(m.group(1))
        for m in re.finditer(r'\btypedef\s+([^;{}]+?)\s*\(\s*\*\s*(\w+)\s*\)\s*\([^;]*\)\s*;', text):
            lint.aliases[m.group(2)] = None
        for m in re.finditer(r'\btypedef\s+([^;{}()]+?)\s+(\w+)\s*;', text):
            lint.aliases[m.group(2)] = m.group(1)
        for m in re.finditer(r'\busing\s+(\w+)\s*=\s*([^;{}]+);', text):
            lint.aliases[m.group(1)] = m.group(2)


VIRTUAL = re.compile(r'\bvirtual\b(?P<ret>.*?)(?P<name>~?\b\w+)\s*\((?P<params>.*)$', re.S)


def check_interface(lint, cls):
    header = cls['header']
    seen = {}
    for st in cls['members']:
        text = re.sub(r'\[\[[^\]]*\]\]', ' ', st.text)
        text = re.sub(r'^\s*(?:(?:public|protected|private)\s*:\s*)+', '', text)
        if not re.search(r'\bvirtual\b', text):
            continue
        lint.virtuals += 1
        m = VIRTUAL.search(text)
        if not m:
            lint.error(header, f'{cls["full"]}: cannot parse virtual declaration: {text}')
            continue
        name = m.group('name')
        if name.startswith('~'):
            lint.error(header, f'{cls["full"]}: R2 virtual destructor ({text})')
            continue
        lp = m.start('params') - 1
        rp = match_close(text, lp, '(', ')')
        params = text[lp + 1:rp]
        quals = text[rp + 1:]
        if not re.search(r'\bnoexcept\b', quals):
            lint.error(header, f'{cls["full"]}::{name}: R5 virtual function is not noexcept')
        if name in seen:
            lint.error(header, f'{cls["full"]}::{name}: R3 overloaded virtual function')
        seen[name] = True
        ret = re.sub(r'\b(inline|static|constexpr)\b', ' ', m.group('ret'))
        bad = resolve_by_value_class(lint, ' '.join(strip_templates(ret).split()))
        if bad:
            lint.error(header, f'{cls["full"]}::{name}: R1 returns {bad} by value')
        for p in split_top(params):
            t = type_of_param(p)
            if t is None:
                continue
            bad = resolve_by_value_class(lint, t)
            if bad:
                lint.error(header, f'{cls["full"]}::{name}: R1 parameter "{" ".join(p.split())}" passes {bad} by value')


FUNC_DECL = re.compile(r'^(?P<pre>[^=(]*?)(?P<name>~?\b\w+|operator\s*[^\s(]+)\s*\(')


def check_class_members(lint, cls, definitions):
    header = cls['header']
    for st in cls['members']:
        text = re.sub(r'\[\[[^\]]*\]\]', ' ', st.text)
        text = re.sub(r'^\s*(?:(?:public|protected|private)\s*:\s*)+', '', text).strip()
        for macro in EXPORT_MACROS:
            if re.search(r'\b%s\b' % macro, text):
                lint.error(header, f'{cls["full"]}: {macro} in a class scope ({text})')
        if st.has_body or not text:
            continue
        if re.match(r'^(friend|using|typedef|static_assert|template\s*<[^>]*>\s*friend)\b', text):
            continue
        if re.search(r'\bvirtual\b', text) or re.search(r'=\s*(0|default|delete)\s*$', text):
            continue
        text = re.sub(r'\balignas\s*\([^()]*\)', ' ', text).strip()
        m = FUNC_DECL.match(re.sub(r'^template\s*<.*?>\s*', '', text))
        if not m:
            continue
        name = m.group('name')
        if re.match(r'^(decltype|sizeof|alignof|noexcept)$', name):
            continue
        if (header, cls['name'], name) in BODYLESS_EXEMPT:
            continue
        if (cls['name'], name) in definitions:
            continue
        lint.error(header, f'{cls["full"]}::{name}: E member function declared without a body ({text})')


def lint_headers(root):
    lint = Lint()
    sources = {}
    for dirpath, _, files in os.walk(root):
        for f in sorted(files):
            if not f.endswith('.h'):
                continue
            rel = os.path.relpath(os.path.join(dirpath, f), root).replace(os.sep, '/')
            if rel in EXCLUDED:
                continue
            with open(os.path.join(dirpath, f), encoding='utf-8', errors='replace') as fh:
                sources[rel] = strip_source(fh.read())
    if not sources:
        lint.error(root, 'no headers found')
        return lint

    collect_types(lint, sources)

    for header, text in sorted(sources.items()):
        classes, definitions = [], set()
        try:
            parse_scope(lint, header, text, 0, len(text), 'namespace', None, classes, definitions)
        except ValueError as e:
            lint.error(header, f'parse error: {e}')
            continue
        api = header in API_HEADERS or header.startswith('common/') or header in R4_EXEMPT
        for cls in classes:
            name = cls['name'] or ''
            interface = re.match(r'^I[A-Z]\w*$', name) is not None
            lint.classes += 1
            if interface:
                lint.interfaces += 1
                check_interface(lint, cls)
            elif api and header not in R4_EXEMPT and cls['bases']:
                lint.error(header, f'{cls["full"]}: R4 data struct with a base class ({cls["bases"]})')
            check_class_members(lint, cls, definitions)
        if api:
            for t in FORBIDDEN_TYPES:
                if re.search(r'\b%s\b' % t, text):
                    lint.error(header, f'T1 module-private type {t} in a public API header')
    return lint


# ---------------------------------------------------------------- self test

PLANTED = [
    ('R1 return', 'nvrhi.h', 'struct IHeap : IRHIObject\n    {',
     'struct IHeap : IRHIObject\n    {\n        virtual MemoryRequirements plantedR1() noexcept = 0;'),
    ('R1 parameter', 'nvrhi.h', 'struct IHeap : IRHIObject\n    {',
     'struct IHeap : IRHIObject\n    {\n        virtual void plantedR1p(TextureSubresourceSet s) noexcept = 0;'),
    ('R1 handle', 'nvrhi.h', 'struct IHeap : IRHIObject\n    {',
     'struct IHeap : IRHIObject\n    {\n        virtual TextureHandle plantedR1h() noexcept = 0;'),
    ('R2', 'nvrhi.h', 'struct IHeap : IRHIObject\n    {',
     'struct IHeap : IRHIObject\n    {\n        virtual ~IHeap() = default;'),
    ('R3', 'nvrhi.h', 'struct IHeap : IRHIObject\n    {',
     'struct IHeap : IRHIObject\n    {\n        virtual void planted(uint32_t a) noexcept = 0;\n        virtual void planted(float a) noexcept = 0;'),
    ('R5', 'nvrhi.h', 'struct IHeap : IRHIObject\n    {',
     'struct IHeap : IRHIObject\n    {\n        virtual void plantedR5() = 0;'),
    ('R4', 'nvrhi.h', 'struct MemoryRequirements\n    {',
     'struct PlantedBase { int a; };\n    struct MemoryRequirements : PlantedBase\n    {'),
    ('T1', 'nvrhi.h', 'struct MemoryRequirements\n    {',
     'struct MemoryRequirements\n    {\n        MonoPtr<int> planted;'),
    ('E export macro', 'nvrhi.h', 'struct MemoryRequirements\n    {',
     'struct MemoryRequirements\n    {\n        NVRHI_API void planted();'),
    ('E bodiless member', 'nvrhi.h', 'struct MemoryRequirements\n    {',
     'struct MemoryRequirements\n    {\n        void planted() const;'),
    ('E bodiless constructor', 'nvrhi.h', 'struct MemoryRequirements\n    {',
     'struct MemoryRequirements\n    {\n        explicit MemoryRequirements(int planted);'),
]


def self_test(root):
    base = lint_headers(root)
    if base.errors:
        print('self-test: the real headers do not pass:')
        for e in base.errors:
            print('  ' + e)
        return 1
    failures = 0
    for name, header, anchor, replacement in PLANTED:
        tmp = tempfile.mkdtemp(prefix='nvrhi_abi_lint_')
        try:
            copy = os.path.join(tmp, 'nvrhi')
            shutil.copytree(root, copy)
            path = os.path.join(copy, header)
            with open(path, encoding='utf-8', newline='') as fh:
                text = fh.read()
            nl = '\r\n' if '\r\n' in text else '\n'
            text = text.replace('\r\n', '\n')
            if anchor not in text:
                print(f'self-test {name}: anchor not found in {header}')
                failures += 1
                continue
            text = text.replace(anchor, replacement, 1)
            with open(path, 'w', encoding='utf-8', newline='') as fh:
                fh.write(text.replace('\n', nl))
            result = lint_headers(copy)
            if result.errors:
                print(f'self-test {name}: detected: {result.errors[0]}')
            else:
                print(f'self-test {name}: NOT detected')
                failures += 1
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    print('self-test: ' + ('FAILED' if failures else f'all {len(PLANTED)} planted violations detected'))
    return 1 if failures else 0


def main(argv):
    if len(argv) == 3 and argv[1] == '--self-test':
        return self_test(argv[2])
    if len(argv) != 2:
        print(__doc__)
        return 2
    lint = lint_headers(argv[1])
    for e in lint.errors:
        print(e)
    if lint.errors:
        print(f'abi_lint: {len(lint.errors)} violation(s)')
        return 1
    print(f'abi_lint: OK ({lint.interfaces} interfaces, {lint.virtuals} virtual functions, '
          f'{lint.classes} classes checked)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
