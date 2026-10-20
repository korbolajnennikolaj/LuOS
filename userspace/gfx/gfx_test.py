import gl

npass = 0
nfail = 0


def check(name, cond):
    global npass, nfail
    if cond:
        npass += 1
        print("PASS " + name)
        gl.log("PASS " + name)
    else:
        nfail += 1
        print("FAIL " + name)
        gl.log("FAIL " + name)


def near(a, b, tol):
    return abs(a - b) <= tol


print("=== LuOS 0.10.56 gfx (MicroPython) - CPU Mesa rendering test ===")

r = gl.start()
check("gl.start", r is not None and not isinstance(r, tuple))
if isinstance(r, tuple):
    print("start error: " + str(r[1]))
else:
    renderer = str(gl.string(gl.RENDERER))
    version = str(gl.string(gl.VERSION))
    print("renderer: " + renderer)
    print("version:  " + version)
    print("vendor:   " + str(gl.string(gl.VENDOR)))
    print("GLSL:     " + str(gl.string(gl.SHADING_LANGUAGE_VERSION)))
    check("renderer is llvmpipe", "llvmpipe" in renderer)
    check("GL version >= 2.1", "2.1" in version or "3." in version or "4." in version)
    check("initial glGetError clean", gl.getError() == 0)

    opened = gl.open(128, 128)
    check("gl.open 128x128", opened == (128, 128))

    gl.clearColor(0.2, 0.4, 0.8, 1.0)
    gl.clear(gl.COLOR_BUFFER_BIT)
    pix = gl.readPixels(0, 0, 128, 128)
    check("readPixels size", pix is not None and len(pix) == 128 * 128 * 4)
    p = gl.pixel(pix, 64, 64, 128)
    print("clear pixel: {} {} {} {}".format(p[0], p[1], p[2], p[3]))
    check("clear color match", near(p[0], 51, 2) and near(p[1], 102, 2) and near(p[2], 204, 2) and p[3] == 255)
    p1 = gl.pixel(pix, 5, 7, 128)
    check("clear color uniform", near(p1[0], 51, 2) and near(p1[1], 102, 2) and near(p1[2], 204, 2))
    check("crc of clear frame nonzero", gl.crc(pix) != 0)
    print("frame crc32: 0x{:08X}".format(gl.crc(pix)))

    gl.clearColor(0, 0, 0, 1)
    gl.clear(gl.COLOR_BUFFER_BIT)
    gl.matrixMode(gl.PROJECTION)
    gl.loadIdentity()
    gl.ortho(-1, 1, -1, 1, -1, 1)
    gl.matrixMode(gl.MODELVIEW)
    gl.loadIdentity()
    gl.begin(gl.TRIANGLES)
    gl.color3f(1, 0, 0)
    gl.vertex2f(-0.6, -0.6)
    gl.color3f(1, 0, 0)
    gl.vertex2f(0.6, -0.6)
    gl.color3f(1, 0, 0)
    gl.vertex2f(0, 0.6)
    gl.end()
    pix = gl.readPixels(0, 0, 128, 128)
    t = gl.pixel(pix, 64, 40, 128)
    print("triangle pixel: {} {} {} {}".format(t[0], t[1], t[2], t[3]))
    check("immediate-mode triangle", t[0] > 240 and t[1] < 15 and t[2] < 15 and t[3] == 255)
    b = gl.pixel(pix, 8, 8, 128)
    check("triangle background", b[0] < 15 and b[1] < 15 and b[2] < 15)
    check("immediate-mode glGetError clean", gl.getError() == 0)

    gl.begin(gl.QUADS)
    gl.color3f(0, 1, 0)
    gl.vertex2f(-0.9, -0.9)
    gl.vertex2f(-0.2, -0.9)
    gl.vertex2f(-0.2, -0.2)
    gl.vertex2f(-0.9, -0.2)
    gl.end()
    pix = gl.readPixels(0, 0, 128, 128)
    q = gl.pixel(pix, 14, 14, 128)
    print("quad pixel: {} {} {}".format(q[0], q[1], q[2]))
    check("immediate-mode quad", q[1] > 240 and q[0] < 15 and q[2] < 15)
    check("quad did not erase triangle", gl.pixel(pix, 64, 40, 128)[0] > 240)

    gl.clearColor(0, 0, 0, 1)
    gl.clear(gl.COLOR_BUFFER_BIT)
    gl.matrixMode(gl.MODELVIEW)
    gl.loadIdentity()
    gl.rotatef(45, 0, 0, 1)
    gl.begin(gl.TRIANGLES)
    gl.color3f(0, 0.5, 1)
    gl.vertex2f(-0.5, -0.5)
    gl.vertex2f(0.5, -0.5)
    gl.vertex2f(0, 0.5)
    gl.end()
    pix = gl.readPixels(0, 0, 128, 128)
    rr = gl.pixel(pix, 64, 64, 128)
    print("rotated triangle center: {} {} {}".format(rr[0], rr[1], rr[2]))
    check("rotated triangle center pixel", rr[0] < 15 and rr[1] > 100 and rr[2] > 240)
    check("glRotatef glGetError clean", gl.getError() == 0)

    vs_src = (
        "#version 120\n"
        "attribute vec2 pos;\n"
        "varying vec2 uv;\n"
        "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n"
    )
    fs_src = (
        "#version 120\n"
        "varying vec2 uv;\n"
        "void main() { gl_FragColor = vec4(uv.x, uv.y, 0.25, 1.0); }\n"
    )

    res = gl.createShader(gl.VERTEX_SHADER, vs_src)
    vs = None if isinstance(res, tuple) else res
    check("vertex shader compiled", vs is not None)
    res = gl.createShader(gl.FRAGMENT_SHADER, fs_src)
    fs = None if isinstance(res, tuple) else res
    check("fragment shader compiled", fs is not None)
    res = gl.createShader(gl.FRAGMENT_SHADER, "#version 120\nvoid main() { gl_FragColor = vec4(bogus; }\n")
    check("bad shader rejected with log", isinstance(res, tuple))

    if vs is not None and fs is not None:
        res = gl.createProgram(vs, fs, ["pos"])
        prog = None if isinstance(res, tuple) else res
        check("program linked", prog is not None)

        if prog is not None:
            vbo = gl.genBuffers(1)
            check("genBuffers", vbo is not None and vbo > 0)
            gl.bindBuffer(gl.ARRAY_BUFFER, vbo)
            gl.bufferData(gl.ARRAY_BUFFER, [-1, -1, 1, -1, -1, 1, 1, 1], gl.STATIC_DRAW)
            gl.useProgram(prog)
            gl.enableVertexAttribArray(0)
            gl.vertexAttribPointer(0, 2, gl.FLOAT, 0, 0, 0)
            gl.clearColor(0, 0, 0, 1)
            gl.clear(gl.COLOR_BUFFER_BIT)
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4)
            pix = gl.readPixels(0, 0, 128, 128)
            u = gl.pixel(pix, 122, 122, 128)
            print("shader gradient pixel: {} {} {} {}".format(u[0], u[1], u[2], u[3]))
            check("shader gradient top-right", u[0] > 230 and u[1] > 230 and near(u[2], 64, 4) and u[3] == 255)
            z = gl.pixel(pix, 2, 2, 128)
            check("shader gradient bottom-left", z[0] < 20 and z[1] < 20 and near(z[2], 64, 4))
            check("shader pipeline glGetError clean", gl.getError() == 0)
            gl.useProgram(0)

    tex = gl.genTextures(1)
    check("genTextures", tex is not None and tex > 0)
    gl.bindTexture(gl.TEXTURE_2D, tex)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST)
    texels = bytearray()
    for y in range(8):
        for x in range(8):
            v = 255 if x < 4 else 40
            texels += bytes([v, 255 - v, 32, 255])
    tr2 = gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 8, 8, gl.RGBA, gl.UNSIGNED_BYTE, bytes(texels))
    check("texImage2D returns 0", tr2 == 0)
    check("texture upload glGetError clean", gl.getError() == 0)

    gl.matrixMode(gl.PROJECTION)
    gl.loadIdentity()
    gl.perspective(60, 1, 0.1, 100)
    gl.matrixMode(gl.MODELVIEW)
    gl.loadIdentity()
    gl.translatef(0.1, 0.2, 0)
    check("matrix stack glGetError clean", gl.getError() == 0)

    gl.matrixMode(gl.MODELVIEW)
    gl.loadIdentity()
    gl.lookAt(0, 0, 3, 0, 0, 0, 0, 1, 0)
    check("lookAt glGetError clean", gl.getError() == 0)

    gl.clearColor(0.1, 0.1, 0.1, 1)
    gl.clear(gl.COLOR_BUFFER_BIT)
    gl.begin(gl.LINES)
    gl.color3f(1, 1, 0)
    gl.vertex2f(-0.5, -0.5)
    gl.vertex2f(0.5, 0.5)
    gl.end()
    gl.begin(gl.POINTS)
    gl.color3f(0, 1, 1)
    gl.vertex2f(0, 0)
    gl.end()
    check("lines and points glGetError clean", gl.getError() == 0)

    check("glFinish", gl.finish() == 0)

    pres = gl.present()
    print("present: " + str(pres))
    check("present frame to screen", pres is True)

    check("final glGetError clean", gl.getError() == 0)

    gl.close()
    check("gl.close", True)

print("RESULT: {} passed, {} failed".format(npass, nfail))
gl.log("RESULT {} passed {} failed".format(npass, nfail))
if nfail == 0:
    print("GFX_PY_TEST_OK")
    gl.log("GFX_PY_TEST_OK")
else:
    print("GFX_PY_TEST_FAILED")
    gl.log("GFX_PY_TEST_FAILED")
