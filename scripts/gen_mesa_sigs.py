#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 korbolajnennikolaj

"""Generates the kernel-side Mesa API tables (src/drivers/GPU/mesa_sigs.c, mesa_gl_defs.h,
mesa_egl_defs.h, mesa_vk_defs.h, mesa_vk_uapi.h) from the Khronos registry headers and the
command lists in src/drivers/GPU/gpu_core.h.

The GL/Vulkan command vocabulary (opcode -> function name) comes from gpu_core.h; argument
signatures come from glcorearb.h (core+ARB), a hand-written table for the GL 1.x..2.x
compatibility functions (they have no modern Khronos header), egl.h/eglext.h and
vulkan_core.h. Vulkan structure layouts are parsed from vulkan_core.h so the kernel-side
copies are ABI-identical to what the Mesa libraries in the built-in filesystem expect.

usage: python3 scripts/gen_mesa_sigs.py [--headers DIR]
"""
import argparse
import os
import re
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src/drivers/GPU")
HDR_URLS = {
    "glcorearb.h": "https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/api/GL/glcorearb.h",
    "gl.h": "https://raw.githubusercontent.com/mesa3d/mesa/mesa-23.3.6/include/GL/gl.h",
    "glext.h": "https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/api/GL/glext.h",
    "vulkan_core.h": "https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/v1.3.290.0/include/vulkan/vulkan_core.h",
    "egl.h": "https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/main/api/EGL/egl.h",
    "eglext.h": "https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/main/api/EGL/eglext.h",
}
GL_CALL_ARGS = 20


def fetch_headers(d):
    os.makedirs(d, exist_ok=True)
    for name, url in HDR_URLS.items():
        p = os.path.join(d, name)
        if os.path.exists(p):
            continue
        print("fetch %s" % name)
        for attempt in range(4):
            try:
                with urllib.request.urlopen(url, timeout=60) as r, open(p + ".part", "wb") as f:
                    f.write(r.read())
                os.rename(p + ".part", p)
                break
            except OSError as e:
                if attempt == 3:
                    raise
                print("retry (%s)" % e)
    return {n: open(os.path.join(d, n), encoding="utf-8", errors="replace").read()
            for n in HDR_URLS}


def parse_cmd_list(text, macro):
    m = re.search(r"#define %s\(X\)(.*?)\n\n" % macro, text, re.S)
    if not m:
        sys.exit("command list %s not found" % macro)
    rows = re.findall(r"X\(([A-Za-z0-9_]+),\s*([a-zA-Z0-9_]+)\)", m.group(1))
    return rows


TYPE_MAP = {
    "GLenum": ("i", 4), "GLboolean": ("i", 1), "GLbitfield": ("i", 4), "GLint": ("i", 4),
    "GLuint": ("i", 4), "GLsizei": ("i", 4), "GLbyte": ("i", 1), "GLshort": ("i", 2),
    "GLubyte": ("i", 1), "GLushort": ("i", 2), "GLclampx": ("i", 4), "GLfixed": ("i", 4),
    "GLint64": ("i", 8), "GLuint64": ("i", 8), "GLintptr": ("i", 8), "GLsizeiptr": ("i", 8),
    "GLfloat": ("f", 4), "GLclampf": ("f", 4), "GLdouble": ("d", 8), "GLclampd": ("d", 8),
    "GLchar": ("i", 1), "GLsync": ("i", 8), "GLDEBUGPROC": ("i", 8), "GLDEBUGPROCKHR": ("i", 8),
    "GLDEBUGPROCAMD": ("i", 8), "GLDEBUGPROCARB": ("i", 8), "void": ("i", 8),
    "GLVULKANPROCNV": ("i", 8), "_GLfuncptr": ("i", 8),
}


def parse_gl_prototypes(text):
    protos = {}
    for m in re.finditer(r"GLAPI\s+([\w\s\*]+?)\s*(?:GL)?APIENTRY\s+(gl\w+)\s*\(([^;]*?)\);",
                         text, re.S):
        ret = m.group(1).strip()
        name = m.group(2)
        args = []
        body = " ".join(m.group(3).split())
        if body not in ("void", ""):
            for part in split_args(body):
                args.append(part.strip())
        protos[name] = (ret, args)
    return protos


def split_args(s):
    out, depth, cur = [], 0, ""
    for c in s:
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += c
    if cur.strip():
        out.append(cur)
    return out


def classify_gl_arg(arg):
    arg = " ".join(arg.split())
    if arg in ("void", ""):
        return None
    m = re.match(r"^(.*?)(\w+)$", arg)
    if not m:
        return None
    ty = m.group(1).strip()
    name = m.group(2)
    ptr = ty.endswith("*")
    ty = ty.replace("*", "").strip()
    const = ty.startswith("const ")
    ty = ty.replace("const ", "").strip()
    base = TYPE_MAP.get(ty)
    if ptr:
        return ("p" if const else "P", name, ty, 8)
    if base:
        return (base[0], name, ty, base[1])
    return ("i", name, ty, 4)


COMPAT_GL = {}

SUFFIX_CLASS = {"b": "i", "s": "i", "i": "i", "f": "f", "d": "d",
                "ub": "i", "us": "i", "ui": "i"}


def compat_family(prefix, counts, kinds, lead=0, with_v=True):
    for n in counts:
        for k in kinds:
            cls = SUFFIX_CLASS[k]
            COMPAT_GL["%s%d%s" % (prefix, n, k)] = "i" * lead + cls * n
            if with_v:
                COMPAT_GL["%s%d%sv" % (prefix, n, k)] = "i" * lead + "p"


compat_family("glColor", (3, 4), ["b", "s", "i", "f", "d", "ub", "us", "ui"])
compat_family("glSecondaryColor", (3,), ["b", "s", "i", "f", "d", "ub", "us", "ui"])
compat_family("glNormal", (3,), ["b", "s", "i", "f", "d"])
compat_family("glVertex", (2, 3, 4), ["b", "s", "i", "f", "d"])
compat_family("glTexCoord", (1, 2, 3, 4), ["b", "s", "i", "f", "d"])
compat_family("glRasterPos", (2, 3, 4), ["b", "s", "i", "f", "d"])
compat_family("glWindowPos", (2, 3), ["b", "s", "i", "f", "d"])
for k in "sifdub":
    COMPAT_GL["glIndex%s" % k] = "i"
    COMPAT_GL["glIndex%sv" % k] = "p"
for n in (1, 2):
    for k in "df":
        COMPAT_GL["glEvalCoord%d%s" % (n, k)] = "i" + k * n
        COMPAT_GL["glEvalCoord%d%sv" % (n, k)] = "i" + "p"
for u in range(1, 5):
    for n in range(1, 5):
        for k in ["b", "s", "i", "f", "d"]:
            cls = SUFFIX_CLASS[k]
            COMPAT_GL["glMultiTexCoord%d%d%s" % (u, n, k)] = "i" + cls * n
            COMPAT_GL["glMultiTexCoord%d%d%sv" % (u, n, k)] = "i" + "p"
COMPAT_GL.update({
    "glAccum": "ifi", "glAlphaFunc": "iff", "glBegin": "i", "glBitmap": "iifiippi",
    "glCallList": "i", "glCallLists": "iip", "glClearAccum": "ffff", "glClearIndex": "f",
    "glClipPlane": "ip", "glColorMask": "iiiii", "glColorMaterial": "ii",
    "glColorPointer": "iiiip", "glCopyPixels": "iiiii", "glCullFace": "i",
    "glDeleteLists": "ii", "glDepthFunc": "i", "glDepthMask": "i", "glDepthRange": "dd",
    "glDrawPixels": "iiipip", "glEdgeFlag": "i", "glEdgeFlagv": "p",
    "glEdgeFlagPointer": "iip", "glEnd": "", "glEndList": "", "glFeedbackBuffer": "iip",
    "glFogCoordd": "d", "glFogCoordf": "f", "glFogCoorddv": "p", "glFogCoordfv": "p",
    "glFogCoordPointer": "iip", "glFrustum": "dddddd", "glGenLists": "i",
    "glInitNames": "", "glIndexPointer": "iip", "glInterleavedArrays": "iip",
    "glIsList": "i", "glLightf": "iif", "glLighti": "iii", "glLightfv": "iip",
    "glLightiv": "iip", "glLightModelf": "if", "glLightModeli": "ii", "glLightModelfv": "ip",
    "glLightModeliv": "ip", "glLineStipple": "ii", "glLineWidth": "f",
    "glListBase": "i", "glLoadIdentity": "", "glLoadMatrixd": "p", "glLoadMatrixf": "p",
    "glLoadName": "i", "glLoadTransposeMatrixd": "p", "glLoadTransposeMatrixf": "p",
    "glLogicOp": "i", "glMap1d": "idiiiddp", "glMap1f": "ifiiiffp", "glMap2d": "idiiiidddp",
    "glMap2f": "ifiiiifffp", "glMapGrid1d": "iddi", "glMapGrid1f": "iffi",
    "glMapGrid2d": "iddiddi", "glMapGrid2f": "iffiffi", "glMaterialf": "iif",
    "glMateriali": "iii", "glMaterialfv": "iip", "glMaterialiv": "iip",
    "glMatrixMode": "i", "glMinSampleShading": "f", "glMultMatrixd": "p",
    "glMultMatrixf": "p", "glMultTransposeMatrixd": "p", "glMultTransposeMatrixf": "p",
    "glNewList": "ii", "glOrtho": "dddddd", "glPassThrough": "f", "glPixelMapfv": "iip",
    "glPixelMapuiv": "iip", "glPixelMapusv": "iip", "glPixelStoref": "if",
    "glPixelStorei": "ii", "glPixelTransferf": "if", "glPixelTransferi": "ii",
    "glPixelZoom": "ff", "glPointParameterf": "if", "glPointParameteri": "ii",
    "glPointParameterfv": "ip", "glPointParameteriv": "ip", "glPointSize": "f",
    "glPolygonMode": "ii", "glPolygonOffset": "ff", "glPolygonStipple": "p",
    "glPopAttrib": "", "glPopClientAttrib": "", "glPopMatrix": "", "glPopName": "",
    "glPrioritizeTextures": "ipp", "glPushAttrib": "i", "glPushClientAttrib": "i",
    "glPushMatrix": "", "glPushName": "i", "glRasterPos2sv": "p", "glReadBuffer": "i",
    "glRectd": "dddd", "glRectf": "ffff", "glRecti": "iiii", "glRects": "iiii",
    "glRectdv": "pp", "glRectfv": "pp", "glRectiv": "pp", "glRectsv": "pp",
    "glRenderMode": "i", "glRotated": "dddd", "glRotatef": "ffff", "glScaled": "ddd",
    "glScalef": "fff", "glScissor": "iiii", "glSelectBuffer": "ip", "glShadeModel": "i",
    "glStencilFunc": "iiii", "glStencilMask": "i", "glStencilOp": "iii",
    "glTexEnvf": "iif", "glTexEnvi": "iii", "glTexEnvfv": "iip", "glTexEnviv": "iip",
    "glTexGend": "iid", "glTexGenf": "iif", "glTexGeni": "iii", "glTexGendv": "iip",
    "glTexGenfv": "iip", "glTexGeniv": "iip", "glTexCoordPointer": "iiiip",
    "glTexSubImage1D": "iiiiiip", "glTranslated": "ddd", "glTranslatef": "fff",
    "glVertexPointer": "iiiip", "glViewport": "iiii",
    "glBlendEquation": "i", "glBlendColor": "ffff", "glBlendFunc": "ii",
    "glClearBuffersv": "iip", "glBlendFuncSeparate": "iiiii",
    "glFenceSync": "ii", "glClientWaitSync": "iii", "glWaitSync": "iii",
    "glDeleteSync": "i", "glIsSync": "i", "glGetSynciv": "iiiip",
    "glSampleCoverage": "fi", "glActiveTexture": "i",
})

GL_OUT_OVERRIDE = {
    "glGenTextures": ("arg", 0, 4), "glGenBuffers": ("arg", 0, 4),
    "glGenFramebuffers": ("arg", 0, 4), "glGenRenderbuffers": ("arg", 0, 4),
    "glGenQueries": ("arg", 0, 4), "glGenSamplers": ("arg", 0, 4),
    "glGenVertexArrays": ("arg", 0, 4), "glGenProgramPipelines": ("arg", 0, 4),
    "glCreateTextures": ("arg", 1, 4), "glCreateBuffers": ("arg", 1, 4),
    "glCreateFramebuffers": ("arg", 1, 4), "glCreateRenderbuffers": ("arg", 1, 4),
    "glCreateQueries": ("arg", 1, 4), "glCreateSamplers": ("arg", 1, 4),
    "glCreateVertexArrays": ("arg", 1, 4), "glCreateProgramPipelines": ("arg", 1, 4),
    "glCreateTransformFeedbacks": ("arg", 1, 4),
    "glDeleteTextures": ("arg", 0, 4), "glDeleteBuffers": ("arg", 0, 4),
    "glDeleteFramebuffers": ("arg", 0, 4), "glDeleteRenderbuffers": ("arg", 0, 4),
    "glDeleteQueries": ("arg", 0, 4), "glDeleteSamplers": ("arg", 0, 4),
    "glDeleteVertexArrays": ("arg", 0, 4), "glDeleteProgramPipelines": ("arg", 0, 4),
    "glDeleteTransformFeedbacks": ("arg", 0, 4),
    "glGenLists": ("ret",), "glCreateProgram": ("ret",), "glCreateShader": ("ret",),
    "glCreateShaderProgramv": ("ret",), "glGetError": ("ret",),
    "glIsEnabled": ("ret",), "glIsTexture": ("ret",), "glIsBuffer": ("ret",),
    "glIsFramebuffer": ("ret",), "glIsRenderbuffer": ("ret",), "glIsQuery": ("ret",),
    "glIsSampler": ("ret",), "glIsVertexArray": ("ret",), "glIsProgram": ("ret",),
    "glIsShader": ("ret",), "glIsProgramPipeline": ("ret",), "glIsList": ("ret",),
    "glIsTransformFeedback": ("ret",), "glIsSync": ("ret",), "glIsEnabledi": ("ret",),
    "glCheckFramebufferStatus": ("ret",), "glFenceSync": ("ret",),
    "glGetString": ("cstr",), "glGetStringi": ("cstr",),
    "glGetUniformLocation": ("ret",), "glGetAttribLocation": ("ret",),
    "glGetUniformBlockIndex": ("ret",), "glGetSubroutineIndex": ("ret",),
    "glGetSubroutineUniformLocation": ("ret",), "glGetProgramResourceIndex": ("ret",),
    "glGetProgramResourceLocation": ("ret",),
    "glGetProgramResourceLocationIndex": ("ret",),
    "glGetGraphicsResetStatus": ("ret",), "glRenderMode": ("ret",),
    "glClientWaitSync": ("ret",), "glUnmapBuffer": ("ret",),
    "glGetDebugMessageLog": ("ret",), "glQueryCounter": ("ret",),
    "glGetQueryObjectiv": ("arg", 1, 4), "glGetQueryObjectuiv": ("arg", 1, 4),
    "glGetQueryObjecti64v": ("arg", 1, 8), "glGetQueryObjectui64v": ("arg", 1, 8),
    "glGetSamplerParameterIiv": ("arg", 1, 4), "glGetQueryiv": ("arg", 1, 4),
    "glGetShaderPrecisionFormat": ("cap", 0), "glShaderSource": ("srcs",),
    "glGetAttachedShaders": ("arg", 1, 4), "glSpecializeShader": ("ret",),
}

GL_STRPTR_ARGS = {"glShaderSource": 2}


def gl_sig_from_proto(name, ret, args):
    if args == ["void"] or not args:
        args = []
    classes = []
    ptrs = []
    for idx, a in enumerate(args):
        c = classify_gl_arg(a)
        if c is None:
            continue
        classes.append(c[0])
        if c[0] in ("p", "P"):
            ptrs.append((idx, c[0]))
    if len(classes) > GL_CALL_ARGS:
        return None
    out = None
    retflag = ret not in ("void",) and ret != "void"
    cstr = ret.startswith("const GLubyte") or ret == "const GLubyte *"
    ov = GL_OUT_OVERRIDE.get(name)
    if ov:
        if ov[0] in ("ret", "cstr"):
            out = ov
        elif ov[0] == "arg":
            out = ("exact", ov[1], ov[2])
        elif ov[0] == "srcs":
            out = ("srcs", 2)
    else:
        for idx, kind in ptrs:
            if kind == "P":
                out = ("cap", idx)
                break
    return ("".join(classes), out, retflag, cstr)


def build_gl_table(gl_cmds, protos):
    table = []
    missing = []
    for cname, fn in gl_cmds:
        if fn.startswith("egl"):
            table.append((fn, None))
            continue
        if fn in protos:
            sig = gl_sig_from_proto(fn, *protos[fn])
            if sig:
                table.append((fn, sig))
                continue
        if fn in COMPAT_GL:
            out = GL_OUT_OVERRIDE.get(fn)
            table.append((fn, (COMPAT_GL[fn], out, out is not None and out[0] == "ret", False)))
            continue
        table.append((fn, None))
        missing.append(fn)
    return table, missing


VK_SCALARS = {
    "uint32_t": 4, "int32_t": 4, "uint64_t": 8, "int64_t": 8, "size_t": 8,
    "float": 4, "double": 8, "VkBool32": 4, "uint8_t": 1, "int8_t": 1,
    "uint16_t": 2, "int16_t": 2, "char": 1, "int": 4,
}


class VkField:
    def __init__(self, ty, name, const, ptr, array, count, inline, bits=0, arrlen=0):
        self.ty, self.name, self.const = ty, name, const
        self.ptr, self.array, self.count = ptr, array, count
        self.inline = inline
        self.bits = bits
        self.arrlen = arrlen
        self.dims = []
        self.off = 0


VK_CONSTS = {}


def dims_of(f):
    out = []
    for d in f.dims if f.dims else []:
        out.append(vk_array_len(d))
    return out


def vk_array_len(expr):
    if expr in ("", None):
        return 1
    if expr.isdigit():
        return int(expr)
    if re.match(r"^0x[0-9a-fA-F]+$", expr):
        return int(expr, 16)
    if expr in VK_CONSTS:
        return VK_CONSTS[expr]
    return 1


def vk_align(size, a):
    return (size + a - 1) & ~(a - 1)


def parse_vk_structs(text):
    structs = {}
    for m in re.finditer(r"^#define\s+(VK_[A-Z0-9_]+)\s+(\d+)[UuLl]*\s*$", text, re.M):
        VK_CONSTS[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"typedef struct (\w+)\s*\{(.*?)\}\s*(\w+);", text, re.S):
        body, name = m.group(2), m.group(3)
        if name in structs:
            continue
        fields = []
        for line in body.split("\n"):
            line = line.split("//")[0].strip()
            if not line or line.startswith("#") or line.startswith("/*"):
                continue
            mm = re.match(r"^(.*?)(\w+)\s*(?::\s*(\d+))?((?:\[\w*\])*);$", line)
            if not mm:
                continue
            decl = mm.group(1).strip()
            fname = mm.group(2)
            bits = int(mm.group(3)) if mm.group(3) else 0
            dims = re.findall(r"\[(\w*)\]", mm.group(4)) if mm.group(4) else []
            arr = dims[0] if dims else None
            const = decl.startswith("const ")
            if const:
                decl = decl[6:].strip()
            ptr = decl.endswith("*")
            decl = decl.replace("*", "").strip()
            fields.append(VkField(decl, fname, const, ptr, arr, None,
                                  not ptr and decl in structs
                                  or not ptr and (decl.startswith("Vk") and decl not in VK_SCALARS),
                                  bits))
            fields[-1].dims = dims
        structs[name] = fields
    sizes = {}

    def layout(name):
        if name in sizes:
            return sizes[name]
        fields = structs[name]
        off, maxa = 0, 1
        bits_left = 0
        for f in fields:
            if f.bits:
                a = sz = 4
                if bits_left <= 0:
                    off = vk_align(off, a)
                    f.off = off
                    off += sz
                    bits_left = 32
                else:
                    f.off = off - 4
                bits_left -= min(bits_left, f.bits)
                maxa = max(maxa, a)
                continue
            if f.ptr:
                a, sz = 8, 8
            elif f.ty in VK_HANDLE_TYPES:
                a, sz = 8, 8
            elif f.ty in VK_SCALARS:
                a = sz = VK_SCALARS[f.ty]
            elif f.ty in ("VkDeviceSize", "VkDeviceAddress"):
                a = sz = 8
            elif f.ty in structs:
                inner = layout(f.ty)
                a = inner[1]
                sz = inner[2]
            else:
                a = sz = 4
            n = 1
            for d in dims_of(f):
                n *= d
            f.arrlen = n
            sz = sz * n
            bits_left = 0
            off = vk_align(off, a)
            f.off = off
            off += sz
            maxa = max(maxa, a)
        off = vk_align(off, maxa)
        sizes[name] = (fields, maxa, off)
        return sizes[name]

    for name in list(structs):
        layout(name)
    return structs, sizes


def parse_vk_prototypes(text):
    protos = {}
    for m in re.finditer(r"VKAPI_ATTR\s+([\w\* ]+?)\s+VKAPI_CALL\s+(vk\w+)\s*\(([^;]*?)\);", text, re.S):
        ret = " ".join(m.group(1).split())
        name = m.group(2)
        body = " ".join(m.group(3).split())
        args = []
        if body and body != "void":
            args = [a.strip() for a in split_args(body)]
        protos[name] = (ret, args)
    return protos


VK_HANDLE_TYPES = set()


def collect_vk_handles(text):
    for m in re.finditer(r"VK_DEFINE_HANDLE\((\w+)\)", text):
        VK_HANDLE_TYPES.add(m.group(1))
    for m in re.finditer(r"VK_DEFINE_NON_DISPATCHABLE_HANDLE\((\w+)\)", text):
        VK_HANDLE_TYPES.add(m.group(1))


def vk_arg_kind(arg, arg_index, args, structs):
    m = re.match(r"^(.*?)(\w+)$", " ".join(arg.split()))
    if not m or m.group(2) in ("void",):
        return None
    decl, name = m.group(1).strip().replace("* ", "*"), m.group(2)
    if decl.startswith("const "):
        decl = decl[6:]
    ptr = decl.endswith("*")
    base = decl.replace("*", "").strip()
    if base.startswith("PFN_"):
        return ("scalar", 8)
    if not ptr:
        if base in VK_HANDLE_TYPES:
            return ("handle", 8)
        return ("scalar", 8 if base in ("VkDeviceSize", "VkDeviceAddress", "size_t") else 4)
    if base == "void":
        if decl.endswith("**"):
            return ("ptr_out_handle", find_count_arg(name, args))
        return ("ptr_opaque", 8)
    if base in VK_HANDLE_TYPES:
        cnt = find_count_arg(name, args)
        return ("ptr_out_handle", cnt)
    if base in structs:
        if not arg.replace(" ", "").startswith("const"):
            cnt = find_count_arg(name, args)
            return ("ptr_out_struct", cnt)
        cnt = find_count_arg(name, args)
        if cnt is not None and name.startswith("p") and name.endswith("s"):
            return ("ptr_struct_array", cnt)
        return ("ptr_struct", None)
    if base in ("char", "uint32_t", "float", "uint64_t", "int32_t", "uint8_t"):
        if arg.replace(" ", "").startswith("const"):
            return ("ptr_scalar_in", None)
        return ("ptr_scalar_out", None)
    return ("ptr_opaque", 8)


def find_count_arg(name, args):
    stem = name[1:] if name.startswith("p") else name
    if stem.endswith("s"):
        stem = stem[:-1]
    if not stem.endswith("Count"):
        stem = stem + "Count"
    for i, a in enumerate(args):
        m = re.match(r"^(?:const\s+)?[\w]+[\s\*]+(\w+)$", " ".join(a.split()))
        if m and m.group(1) in (stem, stem[0].lower() + stem[1:]):
            return i
    return None


EGL_PROTO_SIGS = {
    "eglGetDisplay": "i",
    "eglGetPlatformDisplay": "iipp",
    "eglInitialize": "iPP",
    "eglTerminate": "i",
    "eglBindAPI": "i",
    "eglCreateContext": "iiiip",
    "eglDestroyContext": "ii",
    "eglMakeCurrent": "iiiii",
    "eglQueryString": "ii",
    "eglSwapBuffers": "ii",
    "eglGetProcAddress": "p",
}

MESA_EGL_CMDS = [
    ("MESA_EGL_GET_DISPLAY", "eglGetDisplay"),
    ("MESA_EGL_GET_PLATFORM_DISPLAY", "eglGetPlatformDisplay"),
    ("MESA_EGL_INITIALIZE", "eglInitialize"),
    ("MESA_EGL_TERMINATE", "eglTerminate"),
    ("MESA_EGL_BIND_API", "eglBindAPI"),
    ("MESA_EGL_CREATE_CONTEXT", "eglCreateContext"),
    ("MESA_EGL_DESTROY_CONTEXT", "eglDestroyContext"),
    ("MESA_EGL_MAKE_CURRENT", "eglMakeCurrent"),
    ("MESA_EGL_QUERY_STRING", "eglQueryString"),
    ("MESA_EGL_SWAP_BUFFERS", "eglSwapBuffers"),
    ("MESA_EGL_GET_PROC_ADDRESS", "eglGetProcAddress"),
    ("MESA_PIX", "mesa.pix"),
    ("MESA_TRACK_PIX", "mesa.trackpix"),
    ("MESA_TRACK_POKE", "mesa.trackpoke"),
]

COMPAT_GL_ENUMS = {
    "GL_MODELVIEW": 0x1700, "GL_PROJECTION": 0x1701, "GL_TEXTURE": 0x1702, "GL_COLOR": 0x1800,
    "GL_DEPTH": 0x1801, "GL_STENCIL": 0x1802, "GL_POINTS": 0x0000, "GL_LINES": 0x0001,
    "GL_LINE_LOOP": 0x0002, "GL_LINE_STRIP": 0x0003, "GL_TRIANGLES": 0x0004,
    "GL_TRIANGLE_STRIP": 0x0005, "GL_TRIANGLE_FAN": 0x0006, "GL_QUADS": 0x0007,
    "GL_QUAD_STRIP": 0x0008, "GL_POLYGON": 0x0009, "GL_LIGHTING": 0x0B50,
    "GL_LIGHT0": 0x4000, "GL_LIGHT1": 0x4001, "GL_LIGHT2": 0x4002, "GL_LIGHT3": 0x4003,
    "GL_LIGHT4": 0x4004, "GL_LIGHT5": 0x4005, "GL_LIGHT6": 0x4006, "GL_LIGHT7": 0x4007,
    "GL_COLOR_MATERIAL": 0x0B57, "GL_AMBIENT": 0x1200, "GL_DIFFUSE": 0x1201,
    "GL_SPECULAR": 0x1202, "GL_POSITION": 0x1203, "GL_SPOT_DIRECTION": 0x1204,
    "GL_SPOT_EXPONENT": 0x1205, "GL_SPOT_CUTOFF": 0x1206, "GL_CONSTANT_ATTENUATION": 0x1207,
    "GL_LINEAR_ATTENUATION": 0x1208, "GL_QUADRATIC_ATTENUATION": 0x1209,
    "GL_EMISSION": 0x1600, "GL_SHININESS": 0x1601, "GL_AMBIENT_AND_DIFFUSE": 0x1602,
    "GL_COLOR_INDEXES": 0x1603, "GL_FRONT": 0x0404, "GL_BACK": 0x0405,
    "GL_FRONT_AND_BACK": 0x0408, "GL_FLAT": 0x1D00, "GL_SMOOTH": 0x1D01,
    "GL_COMPILE": 0x1300, "GL_COMPILE_AND_EXECUTE": 0x1301, "GL_RENDER": 0x1C00,
    "GL_FEEDBACK": 0x1C01, "GL_SELECT": 0x1C02, "GL_NORMALIZE": 0x0BA1,
    "GL_AUTO_NORMAL": 0x0D80, "GL_MAP1_COLOR_4": 0x0D90, "GL_MAP1_INDEX": 0x0D91,
    "GL_MAP1_NORMAL": 0x0D92, "GL_MAP1_TEXTURE_COORD_1": 0x0D93,
    "GL_MAP1_TEXTURE_COORD_2": 0x0D94, "GL_MAP1_TEXTURE_COORD_3": 0x0D95,
    "GL_MAP1_TEXTURE_COORD_4": 0x0D96, "GL_MAP1_VERTEX_3": 0x0D97, "GL_MAP1_VERTEX_4": 0x0D98,
    "GL_MAP2_COLOR_4": 0x0DB0, "GL_MAP2_INDEX": 0x0DB1, "GL_MAP2_NORMAL": 0x0DB2,
    "GL_MAP2_TEXTURE_COORD_1": 0x0DB2 + 1, "GL_MAP2_TEXTURE_COORD_2": 0x0DB4,
    "GL_MAP2_TEXTURE_COORD_3": 0x0DB5, "GL_MAP2_TEXTURE_COORD_4": 0x0DB6,
    "GL_MAP2_VERTEX_3": 0x0DB7, "GL_MAP2_VERTEX_4": 0x0DB8, "GL_2D": 0x0600,
    "GL_3D": 0x0601, "GL_3D_COLOR": 0x0602, "GL_3D_COLOR_TEXTURE": 0x0603,
    "GL_4D_COLOR_TEXTURE": 0x0604, "GL_OBJECT_LINEAR": 0x2401, "GL_EYE_LINEAR": 0x2400,
    "GL_SPHERE_MAP": 0x2402, "GL_NORMAL_MAP": 0x2511, "GL_REFLECTION_MAP": 0x8512,
    "GL_EYE_PLANE": 0x2502, "GL_OBJECT_PLANE": 0x2501, "GL_S": 0x2000, "GL_T": 0x2001,
    "GL_R": 0x2002, "GL_Q": 0x2003, "GL_MODULATE": 0x2100, "GL_DECAL": 0x2101,
    "GL_BLEND": 0x0BE2, "GL_ADD": 0x0104, "GL_REPLACE": 0x1E01,
    "GL_TEXTURE_ENV": 0x2300, "GL_TEXTURE_ENV_MODE": 0x2200, "GL_TEXTURE_ENV_COLOR": 0x2201,
    "GL_CLIP_PLANE0": 0x3000, "GL_CLIP_PLANE1": 0x3001, "GL_CLIP_PLANE2": 0x3002,
    "GL_CLIP_PLANE3": 0x3003, "GL_CLIP_PLANE4": 0x3004, "GL_CLIP_PLANE5": 0x3005,
    "GL_ACCUM": 0x0100, "GL_LOAD": 0x0101, "GL_RETURN": 0x0102, "GL_MULT": 0x0103,
    "GL_ALPHA_TEST": 0x0BC0, "GL_ALPHA_TEST_REF": 0x0BC2, "GL_ALPHA_TEST_FUNC": 0x0BC1,
    "GL_INDEX_LOGIC_OP": 0x0BF1, "GL_COLOR_LOGIC_OP": 0x0BF2, "GL_LOGIC_OP_MODE": 0x0BF0,
    "GL_CLIENT_PIXEL_STORE_BIT": 0x00000001, "GL_CLIENT_VERTEX_ARRAY_BIT": 0x00000002,
    "GL_ALL_CLIENT_ATTRIB_BITS": 0xFFFFFFFF, "GL_CURRENT_BIT": 0x00000001,
    "GL_POINT_BIT": 0x00000002, "GL_LINE_BIT": 0x00000004, "GL_POLYGON_BIT": 0x00000008,
    "GL_POLYGON_STIPPLE_BIT": 0x00000010, "GL_PIXEL_MODE_BIT": 0x00000020,
    "GL_LIGHTING_BIT": 0x00000040, "GL_FOG_BIT": 0x00000080, "GL_DEPTH_BUFFER_BIT": 0x00000100,
    "GL_ACCUM_BUFFER_BIT": 0x00000200, "GL_STENCIL_BUFFER_BIT": 0x00000400,
    "GL_VIEWPORT_BIT": 0x00000800, "GL_TRANSFORM_BIT": 0x00001000,
    "GL_ENABLE_BIT": 0x00002000, "GL_COLOR_BUFFER_BIT": 0x00004000,
    "GL_HINT_BIT": 0x00008000, "GL_EVAL_BIT": 0x00010000, "GL_LIST_BIT": 0x00020000,
    "GL_TEXTURE_BIT": 0x00040000, "GL_SCISSOR_BIT": 0x00080000,
    "GL_MULTISAMPLE_BIT": 0x20000000, "GL_ALL_ATTRIB_BITS": 0xFFFFFFFF,
    "GL_VERTEX_ARRAY": 0x8074, "GL_NORMAL_ARRAY": 0x8075, "GL_COLOR_ARRAY": 0x8076,
    "GL_INDEX_ARRAY": 0x8077, "GL_TEXTURE_COORD_ARRAY": 0x8078, "GL_EDGE_FLAG_ARRAY": 0x8079,
    "GL_SELECTION_BUFFER_POINTER": 0x0DF3, "GL_SELECTION_BUFFER_SIZE": 0x0DF4,
    "GL_FEEDBACK_BUFFER_POINTER": 0x0DF1, "GL_FEEDBACK_BUFFER_SIZE": 0x0DF2,
    "GL_FEEDBACK_BUFFER_TYPE": 0x0DF0, "GL_PASS_THROUGH_TOKEN": 0x0800,
    "GL_POINT_TOKEN": 0x0701, "GL_LINE_TOKEN": 0x0702, "GL_POLYGON_TOKEN": 0x0703,
    "GL_BITMAP_TOKEN": 0x0704, "GL_DRAW_PIXEL_TOKEN": 0x0705, "GL_COPY_PIXEL_TOKEN": 0x0706,
    "GL_LINE_RESET_TOKEN": 0x0707, "GL_2_BYTES": 0x1407, "GL_3_BYTES": 0x1408,
    "GL_4_BYTES": 0x1409, "GL_INDEX_SHIFT": 0x0D12, "GL_INDEX_OFFSET": 0x0D13,
    "GL_MAP_COLOR": 0x0D10, "GL_MAP_STENCIL": 0x0D11, "GL_X_SCALE": 0x0D16,
    "GL_Y_SCALE": 0x0D17, "GL_X_BIAS": 0x0D18, "GL_Y_BIAS": 0x0D19,
    "GL_ZOOM_X": 0x0D1A, "GL_ZOOM_Y": 0x0D1B, "GL_RED_SCALE": 0x0D14,
    "GL_GREEN_SCALE": 0x0D18 - 4, "GL_BLUE_SCALE": 0x0D16 - 4, "GL_ALPHA_SCALE": 0x0D1C,
    "GL_RED_BIAS": 0x0D15, "GL_GREEN_BIAS": 0x0D19 - 4, "GL_BLUE_BIAS": 0x0D17 - 4,
    "GL_ALPHA_BIAS": 0x0D1D, "GL_DEPTH_SCALE": 0x0D1E, "GL_DEPTH_BIAS": 0x0D1F,
    "GL_PIXEL_MAP_S_TO_S": 0x0E71,
}


GL_RENAME = {
    "glDepthBounds": "glDepthBoundsdNV",
    "glGetTransformFeedbackBufferiv": "glGetTransformFeedbackiv",
    "glVertexAttrib4ub": "glVertexAttrib4Nub",
    "glVertexAttrib4ui": "glVertexAttribI4ui",
    "glVertexAttrib4us": "glVertexAttrib4Nusv",
    "glVertexDivisor": "glVertexAttribDivisor",
    "glClearBuffersv": None,
    "glVertexAttrib4us": None,
    "glMapBuffer": None,
    "glMapBufferRange": None,
}
VK_RENAME = {
    "vkSignalFence": None,
    "vkGetPhysicalDeviceCalibratedTimestampsEXT": None,
}

VK_ENUM_WHITELIST_ONLY = False


def emit_gl_defs(gl_enums, egl_enums):
    lines = ["#ifndef MESA_GL_DEFS_H", "#define MESA_GL_DEFS_H", "",
             "#define GPU_CMD_GL_EGL_FIRST 0x2F00"]
    for i, (cname, fn) in enumerate(MESA_EGL_CMDS):
        lines.append("#define GPU_CMD_%s (GPU_CMD_GL_EGL_FIRST + %d)" % (cname, i))
    lines.append("#define GPU_CMD_GL_EGL_LAST (GPU_CMD_GL_EGL_FIRST + %d)" % len(MESA_EGL_CMDS))
    lines.append("")
    for name in sorted(gl_enums):
        lines.append("#define %s %s" % (name, gl_enums[name]))
    lines.append("")
    for name in sorted(egl_enums):
        lines.append("#define %s %s" % (name, egl_enums[name]))
    lines += ["", "#endif", ""]
    return "\n".join(lines)


def parse_defines(text, prefix):
    out = {}
    pat = r"^#define\s+(%s[A-Za-z0-9_]+)\s+(0x[0-9A-Fa-f]+|\d+)\s*$" % prefix
    for m in re.finditer(pat, text, re.M):
        out[m.group(1)] = m.group(2)
    return out


def vk_enum_defs(text):
    out = {}
    for m in re.finditer(r"typedef enum\s+(\w+)\s*\{(.*?)\}\s*\w+;", text, re.S):
        for em in re.finditer(r"(VK_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", m.group(2)):
            out[em.group(1)] = em.group(2)
    for m in re.finditer(r"^#define\s+(VK_[A-Z0-9_]+)\s+(\d+)\s*$", text, re.M):
        if m.group(1) not in out:
            out[m.group(1)] = m.group(2)
    return out


structs_global = {}
vk_handle_global = set()


def topo_structs(structs):
    order, seen = [], set()

    def visit(name, stack):
        if name in seen:
            return
        if name in stack:
            return
        stack.add(name)
        for f in structs[name]:
            if not f.ptr and f.ty in structs:
                visit(f.ty, stack)
        stack.discard(name)
        seen.add(name)
        order.append(name)

    for name in sorted(structs):
        visit(name, set())
    return order


TYPE_ALIASES = {
    "uint32_t": "uint32_t", "int32_t": "int32_t", "uint64_t": "uint64_t",
    "int64_t": "int64_t", "float": "float", "double": "double",
    "char": "char", "uint8_t": "uint8_t", "int8_t": "int8_t",
    "uint16_t": "uint16_t", "int16_t": "int16_t", "size_t": "uint64_t",
    "VkBool32": "uint32_t", "VkDeviceSize": "uint64_t",
    "VkDeviceAddress": "uint64_t", "VkFlags": "uint32_t",
    "VkStructureType": "uint32_t", "void": "void",
}


def c_type_for(ty, const, ptr):
    t = ty
    if t.endswith(" const"):
        t = t[:-6].strip()
    base = TYPE_ALIASES.get(t)
    if base is None:
        if t in vk_handle_global:
            if ptr:
                return "uint64_t *"
            return "uint64_t"
        if t in structs_global:
            base = t
        elif t.startswith("PFN_"):
            base = "void *"
            ptr = False
        elif t.endswith("Flags") or t.endswith("FlagBits"):
            base = "uint32_t"
        elif t == "int":
            base = "int32_t"
        else:
            base = "uint32_t"
    if ptr:
        return "%s%s *" % ("const " if const else "", base)
    return base


def emit_vk_uapi(structs, sizes, order, ids):
    lines = ["#ifndef MESA_VK_UAPI_H", "#define MESA_VK_UAPI_H", "",
             "#include <stdint.h>", "", "enum mesa_vk_struct_id {"]
    lines.append("    VK_S_NONE = 0,")
    for name in order:
        lines.append("    VK_S_%s = %d," % (name.upper(), ids[name]))
    lines += ["    VK_S_COUNT", "};", ""]
    for name in order:
        lines.append("typedef struct %s %s;" % (name, name))
    lines.append("")
    for name in order:
        fields, align, size = sizes[name]
        lines.append("struct %s {" % name)
        for f in fields:
            if f.ptr:
                ty = c_type_for(f.ty, f.const, True)
            else:
                ty = c_type_for(f.ty, False, False)
            arr = "[%d]" % f.arrlen if f.array is not None else ""
            if f.bits:
                lines.append("    uint32_t %s : %d;" % (f.name, f.bits))
                continue
            lines.append("    %s%s%s%s;" % (ty, " " if not ty.endswith("*") else "",
                                            f.name, arr))
        lines += ["};", ""]
    lines += ["#endif", ""]
    return "\n".join(lines)


def emit_vk_defs(enums):
    lines = ["#ifndef MESA_VK_DEFS_H", "#define MESA_VK_DEFS_H", ""]
    for name in sorted(enums):
        lines.append("#define %s %s" % (name, enums[name]))
    lines += ["", "#endif", ""]
    return "\n".join(lines)


def build_vk_sig(fn, ret, args, structs, sizes, ids):
    if fn in VK_RENAME and VK_RENAME[fn] is None:
        return None
    if len(args) > 12:
        return None
    kinds = []
    for i, a in enumerate(args):
        k = vk_arg_kind(a, i, args, structs)
        if k is None:
            return None
        kind, extra = k
        strct = 0
        count_arg = 255
        fixed = 0
        m = re.match(r"^(.*?)(\w+)$", " ".join(a.split()))
        decl = m.group(1).strip()
        base = decl.replace("const ", "").replace("*", "").strip()
        if kind == "ptr_struct":
            strct = ids.get(base, 0)
        elif kind == "ptr_struct_array":
            strct = ids.get(base, 0)
            count_arg = extra if extra is not None else 255
        elif kind == "ptr_out_struct":
            strct = ids.get(base, 0)
        elif kind == "ptr_out_handle":
            count_arg = extra if extra is not None else 255
        elif kind == "ptr_scalar_in":
            if base == "char":
                kind = "ptr_str"
            elif fn == "vkCmdSetBlendConstants" or fixed:
                kind, fixed = "ptr_scalar_in", 16
            else:
                kind, fixed = "ptr_scalar_in", 0
        if kind == "scalar":
            if base == "float":
                kind = "scalar_f"
            elif base == "double":
                kind = "scalar_d"
        kinds.append((kind, strct, count_arg, fixed))
    for k in kinds:
        if k[0] == "ptr_opaque" and k[1]:
            return None
    has_ret = 1 if ret != "void" else 0
    return (kinds, has_ret)


VK_KIND_IDS = {
    "scalar": 0, "scalar_f": 0x10, "scalar_d": 0x20, "handle": 1, "ptr_struct": 2,
    "ptr_struct_array": 3, "ptr_out_struct": 4, "ptr_out_handle": 5,
    "ptr_scalar_in": 6, "ptr_scalar_out": 7, "ptr_opaque": 8, "ptr_str": 9,
}


def emit_sigs_c(gl_table, vk_table, structs, sizes, order, ids, hdr):
    L = []
    L.append("#include <stddef.h>")
    L.append("")
    L.append("#include \"drivers/GPU/gpu_core.h\"")
    L.append("#include \"drivers/GPU/mesa.h\"")
    L.append("#include \"drivers/GPU/mesa_sigs.h\"")
    L.append("#include \"drivers/GPU/mesa_vk_uapi.h\"")
    L.append("")
    L.append("const struct mesa_gl_sig mesa_gl_sigs[GPU_CMD_GL_COUNT] = {")
    for i, (fn, sig) in enumerate(gl_table):
        if fn.startswith("egl"):
            L.append("    [%d] = { .name = \"%s\", .sig = { 0 } }," % (i, fn))
            continue
        if sig is None:
            L.append("    [%d] = { .name = NULL }," % i)
            continue
        ffi, out, retflag, cstr = sig
        argc = len(ffi)
        types = []
        for c in ffi:
            types.append({"i": "M_T_I", "f": "M_T_F", "d": "M_T_D",
                          "p": "M_T_P", "P": "M_T_P"}[c])
        out_kind = "M_OUT_NONE"
        out_arg = 255
        out_len_arg = 255
        out_mul = 0
        out2_arg = 255
        out2_mul = 0
        if out and out[0] == "exact" and fn.startswith("glDelete"):
            out = None
        if out:
            if out[0] in ("ret",):
                out_kind = "M_OUT_NONE"
                retflag = True
            elif out[0] == "cstr":
                out_kind, out_arg = "M_OUT_CSTR", 255
            elif out[0] == "cap":
                out_kind, out_arg = "M_OUT_CAP", out[1]
            elif out[0] == "exact":
                out_kind = "M_OUT_EXACT"
                out_arg = ffi.index("P") if "P" in ffi else 255
                out_len_arg, out_mul = out[1], out[2]
            elif out[0] == "srcs":
                out_kind, out_arg = "M_OUT_SRCS", out[1]
        if fn == "glGetShaderPrecisionFormat":
            out_kind, out_arg, out_mul = "M_OUT_EXACT", 2, 8
            out_len_arg, out2_arg, out2_mul = 255, 3, 4
        if fn == "glShaderSource":
            out_len_arg = 1
        L.append("    [%d] = { .name = \"%s\", .argc = %d, .types = { %s },"
                 " .out_kind = M_OUT_%s, .out_arg = %d, .out_len_arg = %d,"
                 " .out_mul = %d, .out2_arg = %d, .out2_mul = %d, .has_ret = %d },"
                 % (i, fn, argc, ", ".join(types), out_kind[6:], out_arg, out_len_arg,
                    out_mul, out2_arg, out2_mul, 1 if retflag else 0))
    L.append("};")
    L.append("")
    L.append("const struct mesa_vk_sig mesa_vk_sigs[GPU_CMD_VK_COUNT] = {")
    for i, (cname, fn) in enumerate(vk_table):
        real = VK_RENAME.get(fn, fn)
        if real is None:
            L.append("    [%d] = { .argc = 0 }," % i)
            continue
        proto = hdr["vkprotos"].get(real)
        if proto is None:
            L.append("    [%d] = { .argc = 0 }," % i)
            continue
        sig = build_vk_sig(real, proto[0], proto[1], structs, sizes, ids)
        if sig is None:
            L.append("    [%d] = { .argc = 0 }," % i)
            continue
        kinds, has_ret = sig
        args_c = []
        for kind, strct, count_arg, fixed in kinds:
            args_c.append("{ %d, %d, %d, %d }" % (VK_KIND_IDS[kind], strct, count_arg, fixed))
        L.append("    [%d] = { .argc = %d, .has_ret = %d, .a = { %s } },"
                 % (i, len(kinds), has_ret, ", ".join(args_c)))
    L.append("};")
    L.append("")
    L.append("const struct mesa_vk_struct mesa_vk_structs[VK_S_COUNT] = {")
    for name in order:
        fields, align, size = sizes[name]
        flds = []
        for f in fields:
            if f.ptr:
                kind = 1 if f.ty in structs or f.ty == "void" or f.ty == "char" else 1
                strct = ids.get(f.ty, 0)
                flds.append("{ %d, %d, 1 }" % (f.off, strct))
            elif f.ty in structs:
                flds.append("{ %d, %d, 2 }" % (f.off, ids[f.ty]))
        L.append("    [VK_S_%s] = { %d, %d, (const struct mesa_vk_fld[]){ %s } },"
                 % (name.upper(), size, len(flds), ", ".join(flds) if flds else "0"))
    L.append("};")
    L.append("")
    return "\n".join(L)


def main():
    global structs_global
    ap = argparse.ArgumentParser()
    ap.add_argument("--headers", default=os.path.join(ROOT, "scripts", "khronos-headers"))
    args = ap.parse_args()
    text = fetch_headers(args.headers)
    core = open(os.path.join(ROOT, "src/drivers/GPU/gpu_core.h")).read()
    gl_cmds = parse_cmd_list(core, "GPU_GL_CMD_LIST")
    vk_cmds = parse_cmd_list(core, "GPU_VK_CMD_LIST")

    protos = {}
    protos.update(parse_gl_prototypes(text["glcorearb.h"]))
    for name, p in parse_gl_prototypes(text["glext.h"]).items():
        protos.setdefault(name, p)
    for name, p in parse_gl_prototypes(text["gl.h"]).items():
        protos.setdefault(name, p)

    gl_table, missing = build_gl_table(gl_cmds, protos)
    for i, (fn, sig) in enumerate(gl_table):
        if fn in GL_RENAME:
            real = GL_RENAME[fn]
            if real is None:
                gl_table[i] = (fn, None)
            elif real in protos:
                gl_table[i] = (real, gl_sig_from_proto(real, *protos[real]))
            elif real in COMPAT_GL:
                gl_table[i] = (real, (COMPAT_GL[real], None, False, False))
    still = [fn for fn, sig in gl_table if sig is None and not fn.startswith("egl")]
    print("GL commands: %d, unsupported: %d %s" % (len(gl_table), len(still), still))

    collect_vk_handles(text["vulkan_core.h"])
    vkprotos = parse_vk_prototypes(text["vulkan_core.h"])
    structs, sizes = parse_vk_structs(text["vulkan_core.h"])
    vk_handle_global.update(VK_HANDLE_TYPES)
    structs_global = structs
    order = topo_structs(structs)
    ids = {n: i + 1 for i, n in enumerate(order)}

    gl_enums = {}
    for h in ("gl.h", "glcorearb.h", "glext.h"):
        for k, v in parse_defines(text[h], "GL_").items():
            gl_enums.setdefault(k, v)
    egl_enums = {}
    for h in ("egl.h", "eglext.h"):
        for k, v in parse_defines(text[h], "EGL_").items():
            egl_enums.setdefault(k, v)

    hdr = {"vkprotos": vkprotos}
    sigc = emit_sigs_c(gl_table, vk_cmds, structs, sizes, order, ids, hdr)
    with open(os.path.join(OUT, "mesa_sigs.c"), "w") as f:
        f.write(sigc)
    with open(os.path.join(OUT, "mesa_gl_defs.h"), "w") as f:
        f.write(emit_gl_defs(gl_enums, egl_enums))
    with open(os.path.join(OUT, "mesa_vk_defs.h"), "w") as f:
        f.write(emit_vk_defs(vk_enum_defs(text["vulkan_core.h"])))
    with open(os.path.join(OUT, "mesa_vk_uapi.h"), "w") as f:
        f.write(emit_vk_uapi(structs, sizes, order, ids))
    vk_supported = sum(1 for cname, fn in vk_cmds
                       if VK_RENAME.get(fn, fn) in vkprotos)
    print("VK commands: %d, with prototypes: %d" % (len(vk_cmds), vk_supported))
    print("VK structs: %d" % len(order))
    print("GL enums: %d, EGL enums: %d" % (len(gl_enums), len(egl_enums)))


if __name__ == "__main__":
    main()
